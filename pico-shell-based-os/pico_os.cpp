#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "pico/util/datetime.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "lwip/apps/sntp.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"

// LittleFS includes
extern "C" {
#include "lfs.h"
}

// System configuration
#define MAX_COMMAND_LEN 256
#define MAX_ARGS 16
#define MAX_PROCESSES 8
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - (512 * 1024)) // Last 512KB for filesystem
#define LFS_BLOCK_SIZE 4096
#define LFS_BLOCK_COUNT 128

// Web server configuration
#define HTTP_SERVER_PORT 80
#define MAX_HTTP_CONNECTIONS 4
#define HTTP_BUFFER_SIZE 1536

// ANSI color codes for terminal
#define ANSI_RESET "\033[0m"
#define ANSI_BOLD "\033[1m"
#define ANSI_RED "\033[31m"
#define ANSI_GREEN "\033[32m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_BLUE "\033[34m"
#define ANSI_MAGENTA "\033[35m"
#define ANSI_CYAN "\033[36m"
#define ANSI_CLEAR_SCREEN "\033[2J\033[H"

// Tetris configuration
#define TETRIS_WIDTH 10
#define TETRIS_HEIGHT 20
#define TETRIS_PREVIEW_SIZE 4

// Snake configuration
#define SNAKE_WIDTH 20
#define SNAKE_HEIGHT 15
#define SNAKE_MAX_LENGTH 100

// Process structure
struct Process {
    char name[32];
    bool running;
    uint32_t start_time;
    void (*func)(void);
};

// Todo item structure
struct TodoItem {
    char text[100];
    bool completed;
    bool active;
};

// Tetris piece structure
struct TetrisPiece {
    int shape[4][4];
    int x, y;
    int type;
};

// Snake structure
struct SnakeSegment {
    int x, y;
};

// HTTP connection state
struct http_conn_state {
    struct pbuf *p;
    int sent;
    bool close_after_send;
};

// Global variables
static char command_buffer[MAX_COMMAND_LEN];
static int cmd_index = 0;
static Process processes[MAX_PROCESSES];
static int process_count = 0;
static absolute_time_t boot_time;
static bool wifi_connected = false;
static char wifi_ssid[64] = "";
static char wifi_password[64] = "";
static char timezone_str[32] = "GMT";
static int timezone_offset = 0; // UK timezone (will be +1 during BST)
static bool ntp_synced = false;
static time_t system_time_offset = 0; // Offset from boot time to actual time
static absolute_time_t time_sync_base; // When we last synced time

// Web server globals
static bool http_server_running = false;
static struct tcp_pcb *http_server_pcb = NULL;
static int active_connections = 0;

// LittleFS variables
static lfs_t lfs;
static struct lfs_config lfs_cfg;
static uint8_t lfs_read_buffer[LFS_BLOCK_SIZE];
static uint8_t lfs_prog_buffer[LFS_BLOCK_SIZE];
static uint8_t lfs_lookahead_buffer[128];

// Log system
#define MAX_LOG_ENTRIES 50
static char log_entries[MAX_LOG_ENTRIES][128];
static int log_index = 0;
static int log_count = 0;

// Todo list
static TodoItem todos[2] = {
    {"", false, false},
    {"", false, false}
};

// Forward declarations
void shell_loop();
void log_message(const char* msg);
void print_prompt();
void execute_command(char* cmd);
char* read_line(const char* prompt, bool echo);
void start_http_server();
void stop_http_server();
void create_default_website();

// Panic handler - prints panic info over USB serial
void panic_handler(const char *fmt, ...) {
    // Try to print panic message over USB
    printf("\r\n\r\n");
    printf("╔════════════════════════════════════════╗\r\n");
    printf("║           SYSTEM PANIC!                ║\r\n");
    printf("╚════════════════════════════════════════╝\r\n");
    printf("\r\n");
    
    va_list args;
    va_start(args, fmt);
    printf("PANIC: ");
    vprintf(fmt, args);
    printf("\r\n");
    va_end(args);
    
    printf("\r\n");
    printf("System halted. Please reboot (unplug/replug).\r\n");
    printf("\r\n");
    
    // Flash LED rapidly to indicate panic
    while (true) {
        busy_wait_ms(100);
    }
}

// Helper function to get current time
time_t get_current_time() {
    if (system_time_offset == 0) {
        return 0; // No time set yet
    }
    uint64_t elapsed_ms = absolute_time_diff_us(time_sync_base, get_absolute_time()) / 1000;
    return system_time_offset + (elapsed_ms / 1000);
}

// Helper function to set current time
void set_current_time(time_t new_time) {
    system_time_offset = new_time;
    time_sync_base = get_absolute_time();
    ntp_synced = true;
}

// LittleFS flash operations
int lfs_flash_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size) {
    uint32_t addr = FLASH_TARGET_OFFSET + (block * c->block_size) + off;
    memcpy(buffer, (uint8_t*)XIP_BASE + addr, size);
    return 0;
}

int lfs_flash_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size) {
    uint32_t addr = FLASH_TARGET_OFFSET + (block * c->block_size) + off;
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(addr, (const uint8_t*)buffer, size);
    restore_interrupts(ints);
    return 0;
}

int lfs_flash_erase(const struct lfs_config *c, lfs_block_t block) {
    uint32_t addr = FLASH_TARGET_OFFSET + (block * c->block_size);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(addr, c->block_size);
    restore_interrupts(ints);
    return 0;
}

int lfs_flash_sync(const struct lfs_config *c) {
    return 0;
}

// Initialize LittleFS
void init_filesystem() {
    lfs_cfg.read = lfs_flash_read;
    lfs_cfg.prog = lfs_flash_prog;
    lfs_cfg.erase = lfs_flash_erase;
    lfs_cfg.sync = lfs_flash_sync;
    lfs_cfg.read_size = 1;
    lfs_cfg.prog_size = FLASH_PAGE_SIZE;
    lfs_cfg.block_size = LFS_BLOCK_SIZE;
    lfs_cfg.block_count = LFS_BLOCK_COUNT;
    lfs_cfg.cache_size = 256;
    lfs_cfg.lookahead_size = 128;
    lfs_cfg.block_cycles = 500;
    lfs_cfg.read_buffer = lfs_read_buffer;
    lfs_cfg.prog_buffer = lfs_prog_buffer;
    lfs_cfg.lookahead_buffer = lfs_lookahead_buffer;

    int err = lfs_mount(&lfs, &lfs_cfg);
    if (err) {
        printf("Formatting filesystem...\r\n");
        err = lfs_format(&lfs, &lfs_cfg);
        if (err) {
            printf("ERROR: Failed to format filesystem (code %d)\r\n", err);
            log_message("ERROR: Filesystem format failed");
            return;
        }
        err = lfs_mount(&lfs, &lfs_cfg);
        if (err) {
            printf("ERROR: Failed to mount filesystem after format (code %d)\r\n", err);
            log_message("ERROR: Filesystem mount failed");
            return;
        }
    }
    
    log_message("Filesystem mounted successfully");
}

// Log message function
void log_message(const char* msg) {
    time_t now = get_current_time();
    if (now == 0) {
        // No time set yet, use uptime
        uint32_t uptime_sec = to_ms_since_boot(get_absolute_time()) / 1000;
        snprintf(log_entries[log_index], sizeof(log_entries[0]), 
                 "[+%05lus] %s", (unsigned long)uptime_sec, msg);
    } else {
        struct tm *t = localtime(&now);
        snprintf(log_entries[log_index], sizeof(log_entries[0]), 
                 "[%02d:%02d:%02d] %s", t->tm_hour, t->tm_min, t->tm_sec, msg);
    }
    log_index = (log_index + 1) % MAX_LOG_ENTRIES;
    if (log_count < MAX_LOG_ENTRIES) log_count++;
}

// NTP time sync callback
void ntp_recv_callback(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    if (p->tot_len >= 48) {
        uint8_t *data = (uint8_t*)p->payload;
        uint32_t ntp_time = (data[40] << 24) | (data[41] << 16) | (data[42] << 8) | data[43];
        time_t unix_time = ntp_time - 2208988800UL + (timezone_offset * 3600); // NTP to Unix epoch with timezone
        
        set_current_time(unix_time);
        log_message("NTP time synchronized");
        printf(ANSI_GREEN "Time synchronized successfully!\n" ANSI_RESET);
    }
    pbuf_free(p);
}

void sync_ntp_time() {
    if (!wifi_connected) {
        printf(ANSI_YELLOW "WiFi not connected. Cannot sync time.\n" ANSI_RESET);
        return;
    }
    
    printf("Syncing time with NTP server...\n");
    
    struct udp_pcb *pcb = udp_new();
    if (!pcb) {
        printf(ANSI_RED "Failed to create UDP socket\n" ANSI_RESET);
        return;
    }
    
    ip_addr_t ntp_server;
    if (!ipaddr_aton("129.6.15.28", &ntp_server)) { // NIST time server
        printf(ANSI_RED "Failed to resolve NTP server\n" ANSI_RESET);
        udp_remove(pcb);
        return;
    }
    
    udp_recv(pcb, ntp_recv_callback, NULL);
    
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 48, PBUF_RAM);
    if (!p) {
        printf(ANSI_RED "Failed to allocate buffer\n" ANSI_RESET);
        udp_remove(pcb);
        return;
    }
    
    uint8_t *req = (uint8_t*)p->payload;
    memset(req, 0, 48);
    req[0] = 0x1B; // LI = 0, VN = 3, Mode = 3
    
    cyw43_arch_lwip_begin();
    err_t err = udp_sendto(pcb, p, &ntp_server, 123);
    cyw43_arch_lwip_end();
    
    pbuf_free(p);
    
    if (err != ERR_OK) {
        printf(ANSI_RED "Failed to send NTP request\n" ANSI_RESET);
        udp_remove(pcb);
        return;
    }
    
    // Wait for response (simple timeout)
    sleep_ms(2000);
    udp_remove(pcb);
}

// ===== WEB SERVER - HTTP SERVER FROM LITTLEFS =====

// Get MIME type from file extension
const char* get_mime_type(const char* filename) {
    const char* ext = strrchr(filename, '.');
    if (!ext) return "text/plain";
    
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    if (strcmp(ext, ".txt") == 0) return "text/plain";
    
    return "application/octet-stream";
}

// HTTP error response
err_t send_http_error(struct tcp_pcb *pcb, int code, const char* message) {
    char response[256];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body><h1>%d %s</h1></body></html>\r\n",
        code, message, code, message);
    
    cyw43_arch_lwip_begin();
    err_t err = tcp_write(pcb, response, len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    cyw43_arch_lwip_end();
    
    return err;
}

// Send file from LittleFS
err_t send_file_response(struct tcp_pcb *pcb, const char* filepath) {
    lfs_file_t file;
    int result = lfs_file_open(&lfs, &file, filepath, LFS_O_RDONLY);
    
    if (result < 0) {
        log_message("HTTP: File not found");
        return send_http_error(pcb, 404, "Not Found");
    }
    
    // Get file size
    lfs_soff_t size = lfs_file_size(&lfs, &file);
    if (size < 0 || size > 65536) { // Max 64KB per file
        lfs_file_close(&lfs, &file);
        return send_http_error(pcb, 500, "Internal Server Error");
    }
    
    // Allocate buffer for file content
    char *content = (char*)malloc(size + 1);
    if (!content) {
        lfs_file_close(&lfs, &file);
        return send_http_error(pcb, 500, "Internal Server Error");
    }
    
    // Read file
    lfs_ssize_t read_size = lfs_file_read(&lfs, &file, content, size);
    lfs_file_close(&lfs, &file);
    
    if (read_size < 0) {
        free(content);
        return send_http_error(pcb, 500, "Internal Server Error");
    }
    
    content[read_size] = '\0';
    
    // Build HTTP response header
    const char* mime = get_mime_type(filepath);
    char header[256];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        mime, (int)read_size);
    
    // Send response
    cyw43_arch_lwip_begin();
    err_t err = tcp_write(pcb, header, header_len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK) {
        err = tcp_write(pcb, content, read_size, TCP_WRITE_FLAG_COPY);
    }
    tcp_output(pcb);
    cyw43_arch_lwip_end();
    
    free(content);
    
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "HTTP: Served %s (%d bytes)", filepath, (int)read_size);
    log_message(log_msg);
    
    return err;
}

// HTTP request received callback
err_t http_recv_callback(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    if (p == NULL) {
        // Connection closed
        active_connections--;
        tcp_close(pcb);
        return ERR_OK;
    }
    
    if (err != ERR_OK) {
        pbuf_free(p);
        active_connections--;
        tcp_close(pcb);
        return err;
    }
    
    // Parse HTTP request
    char request[512];
    int len = p->tot_len < sizeof(request) - 1 ? p->tot_len : sizeof(request) - 1;
    pbuf_copy_partial(p, request, len, 0);
    request[len] = '\0';
    
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    
    // Extract request path
    char method[16], path[128], version[16];
    if (sscanf(request, "%15s %127s %15s", method, path, version) != 3) {
        send_http_error(pcb, 400, "Bad Request");
        active_connections--;
        tcp_close(pcb);
        return ERR_OK;
    }
    
    // Log request
    char log_msg[256];
    snprintf(log_msg, sizeof(log_msg), "HTTP: %s %s", method, path);
    log_message(log_msg);
    
    // Handle request
    if (strcmp(method, "GET") != 0) {
        send_http_error(pcb, 405, "Method Not Allowed");
    } else {
        // Map URL to file
        char filepath[144];
        if (strcmp(path, "/") == 0) {
            strcpy(filepath, "/web/index.html");
        } else {
            snprintf(filepath, sizeof(filepath), "/web%s", path);
        }
        
        send_file_response(pcb, filepath);
    }
    
    // Close connection
    sleep_ms(10);
    active_connections--;
    tcp_close(pcb);
    
    return ERR_OK;
}

// HTTP connection error callback
void http_err_callback(void *arg, err_t err) {
    if (active_connections > 0) {
        active_connections--;
    }
}

// New connection accepted
err_t http_accept_callback(void *arg, struct tcp_pcb *newpcb, err_t err) {
    if (err != ERR_OK || newpcb == NULL) {
        return ERR_VAL;
    }
    
    if (active_connections >= MAX_HTTP_CONNECTIONS) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }
    
    active_connections++;
    
    tcp_setprio(newpcb, TCP_PRIO_MIN);
    tcp_arg(newpcb, NULL);
    tcp_recv(newpcb, http_recv_callback);
    tcp_err(newpcb, http_err_callback);
    
    return ERR_OK;
}

// Start HTTP server
void start_http_server() {
    if (!wifi_connected) {
        printf(ANSI_RED "Error: WiFi not connected!\n" ANSI_RESET);
        printf("Connect to WiFi first using the 'wifi' command\n");
        return;
    }
    
    if (http_server_running) {
        printf(ANSI_YELLOW "Web server is already running\n" ANSI_RESET);
        return;
    }
    
    // Create listening PCB
    http_server_pcb = tcp_new();
    if (!http_server_pcb) {
        printf(ANSI_RED "Failed to create server PCB\n" ANSI_RESET);
        return;
    }
    
    err_t err = tcp_bind(http_server_pcb, IP_ADDR_ANY, HTTP_SERVER_PORT);
    if (err != ERR_OK) {
        printf(ANSI_RED "Failed to bind to port %d\n" ANSI_RESET, HTTP_SERVER_PORT);
        tcp_close(http_server_pcb);
        http_server_pcb = NULL;
        return;
    }
    
    http_server_pcb = tcp_listen(http_server_pcb);
    tcp_accept(http_server_pcb, http_accept_callback);
    
    http_server_running = true;
    active_connections = 0;
    
    // Get IP address
    char ip_str[16];
    const ip4_addr_t *addr = netif_ip4_addr(netif_list);
    snprintf(ip_str, sizeof(ip_str), "%s", ip4addr_ntoa(addr));
    
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_GREEN);
    printf("╔═══════════════════════════════════════════════╗\n");
    printf("║        WEB SERVER STARTED - VERSION 2.0       ║\n");
    printf("╚═══════════════════════════════════════════════╝\n");
    printf(ANSI_RESET "\n");
    
    printf(ANSI_BOLD "Server Status:\n" ANSI_RESET);
    printf("  • Running on:    http://%s:%d\n", ip_str, HTTP_SERVER_PORT);
    printf("  • Document root: /web/ (on LittleFS)\n");
    printf("  • Max connections: %d\n\n", MAX_HTTP_CONNECTIONS);
    
    printf(ANSI_CYAN "How to access:\n" ANSI_RESET);
    printf("  1. Open a web browser on your device\n");
    printf("  2. Navigate to: " ANSI_BOLD "http://%s" ANSI_RESET "\n", ip_str);
    printf("  3. Your HTML/CSS files from /web/ will be served\n\n");
    
    printf(ANSI_YELLOW "Quick Start:\n" ANSI_RESET);
    printf("  • Create HTML files: nano /web/index.html\n");
    printf("  • Create CSS files:  nano /web/style.css\n");
    printf("  • List web files:    ls\n");
    printf("  • Stop server:       Press Ctrl+C or type 'stopweb'\n\n");
    
    log_message("HTTP server started");
    
    printf(ANSI_GREEN "Server is running! Access it from your browser.\n" ANSI_RESET);
    printf("Type 'stopweb' to stop the server, or any command to continue.\n\n");
}

// Stop HTTP server
void stop_http_server() {
    if (!http_server_running) {
        printf(ANSI_YELLOW "Web server is not running\n" ANSI_RESET);
        return;
    }
    
    if (http_server_pcb) {
        tcp_close(http_server_pcb);
        http_server_pcb = NULL;
    }
    
    http_server_running = false;
    active_connections = 0;
    
    printf(ANSI_GREEN "Web server stopped\n" ANSI_RESET);
    log_message("HTTP server stopped");
}

// Create default website files
void create_default_website() {
    printf("Creating default website in /web/...\n");
    
    // Create /web directory
    lfs_mkdir(&lfs, "/web");
    
    // Create index.html
    lfs_file_t file;
    const char* index_html = 
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>Pico OS v2.0 - Web Server</title>\n"
        "    <link rel=\"stylesheet\" href=\"style.css\">\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"container\">\n"
        "        <header>\n"
        "            <h1>🚀 Welcome to Pico OS v2.0</h1>\n"
        "            <p class=\"subtitle\">Raspberry Pi Pico 2 W Web Server</p>\n"
        "        </header>\n"
        "        \n"
        "        <main>\n"
        "            <div class=\"card\">\n"
        "                <h2>✨ Features</h2>\n"
        "                <ul>\n"
        "                    <li>HTTP Web Server running on Pico 2 W</li>\n"
        "                    <li>HTML & CSS support from LittleFS flash</li>\n"
        "                    <li>Dual-core processing architecture</li>\n"
        "                    <li>Real-time file system storage</li>\n"
        "                </ul>\n"
        "            </div>\n"
        "            \n"
        "            <div class=\"card\">\n"
        "                <h2>📝 Getting Started</h2>\n"
        "                <p>Edit this page using the nano editor:</p>\n"
        "                <code>nano /web/index.html</code>\n"
        "                <p>Customize the CSS stylesheet:</p>\n"
        "                <code>nano /web/style.css</code>\n"
        "            </div>\n"
        "            \n"
        "            <div class=\"card\">\n"
        "                <h2>💡 System Info</h2>\n"
        "                <p><strong>Platform:</strong> Raspberry Pi Pico 2 W</p>\n"
        "                <p><strong>RAM:</strong> 520 KB</p>\n"
        "                <p><strong>Flash:</strong> 512 KB (for filesystem)</p>\n"
        "                <p><strong>WiFi:</strong> 2.4 GHz 802.11n</p>\n"
        "            </div>\n"
        "        </main>\n"
        "        \n"
        "        <footer>\n"
        "            <p>Pico OS Version 2.0 | Powered by LittleFS & lwIP</p>\n"
        "        </footer>\n"
        "    </div>\n"
        "</body>\n"
        "</html>\n";
    
    if (lfs_file_open(&lfs, &file, "/web/index.html", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) >= 0) {
        lfs_file_write(&lfs, &file, index_html, strlen(index_html));
        lfs_file_close(&lfs, &file);
        printf("[OK] Created /web/index.html\n");
    }
    
    // Create style.css
    const char* style_css = 
        "* {\n"
        "    margin: 0;\n"
        "    padding: 0;\n"
        "    box-sizing: border-box;\n"
        "}\n"
        "\n"
        "body {\n"
        "    font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);\n"
        "    min-height: 100vh;\n"
        "    display: flex;\n"
        "    justify-content: center;\n"
        "    align-items: center;\n"
        "    padding: 20px;\n"
        "}\n"
        "\n"
        ".container {\n"
        "    max-width: 800px;\n"
        "    background: white;\n"
        "    border-radius: 20px;\n"
        "    box-shadow: 0 20px 60px rgba(0,0,0,0.3);\n"
        "    overflow: hidden;\n"
        "}\n"
        "\n"
        "header {\n"
        "    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);\n"
        "    color: white;\n"
        "    padding: 40px;\n"
        "    text-align: center;\n"
        "}\n"
        "\n"
        "header h1 {\n"
        "    font-size: 2.5em;\n"
        "    margin-bottom: 10px;\n"
        "}\n"
        "\n"
        ".subtitle {\n"
        "    font-size: 1.2em;\n"
        "    opacity: 0.9;\n"
        "}\n"
        "\n"
        "main {\n"
        "    padding: 40px;\n"
        "}\n"
        "\n"
        ".card {\n"
        "    background: #f8f9fa;\n"
        "    border-radius: 10px;\n"
        "    padding: 25px;\n"
        "    margin-bottom: 20px;\n"
        "}\n"
        "\n"
        ".card h2 {\n"
        "    color: #667eea;\n"
        "    margin-bottom: 15px;\n"
        "    font-size: 1.5em;\n"
        "}\n"
        "\n"
        ".card ul {\n"
        "    list-style: none;\n"
        "    padding-left: 0;\n"
        "}\n"
        "\n"
        ".card li {\n"
        "    padding: 8px 0;\n"
        "    padding-left: 25px;\n"
        "    position: relative;\n"
        "}\n"
        "\n"
        ".card li:before {\n"
        "    content: '✓';\n"
        "    position: absolute;\n"
        "    left: 0;\n"
        "    color: #667eea;\n"
        "    font-weight: bold;\n"
        "}\n"
        "\n"
        "code {\n"
        "    background: #2d3748;\n"
        "    color: #68d391;\n"
        "    padding: 8px 12px;\n"
        "    border-radius: 5px;\n"
        "    display: block;\n"
        "    margin: 10px 0;\n"
        "    font-family: 'Courier New', monospace;\n"
        "}\n"
        "\n"
        "footer {\n"
        "    background: #2d3748;\n"
        "    color: white;\n"
        "    text-align: center;\n"
        "    padding: 20px;\n"
        "    font-size: 0.9em;\n"
        "}\n";
    
    if (lfs_file_open(&lfs, &file, "/web/style.css", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) >= 0) {
        lfs_file_write(&lfs, &file, style_css, strlen(style_css));
        lfs_file_close(&lfs, &file);
        printf("[OK] Created /web/style.css\n");
    }
    
    printf(ANSI_GREEN "\nDefault website created successfully!\n" ANSI_RESET);
    printf("Start the web server with: " ANSI_BOLD "localhost\n" ANSI_RESET);
}

// Process management
int add_process(const char* name, void (*func)(void)) {
    if (process_count >= MAX_PROCESSES) {
        return -1;
    }
    
    if (!name || !func) {
        return -1;
    }
    
    strncpy(processes[process_count].name, name, sizeof(processes[process_count].name) - 1);
    processes[process_count].name[sizeof(processes[process_count].name) - 1] = '\0';
    processes[process_count].running = true;
    processes[process_count].start_time = to_ms_since_boot(get_absolute_time()) / 1000;
    processes[process_count].func = func;
    
    // Launch on core 1
    multicore_reset_core1();
    multicore_launch_core1(func);
    
    log_message("Process started");
    return process_count++;
}

void list_processes() {
    printf("\n" ANSI_BOLD "Running Processes:" ANSI_RESET "\n");
    printf("%-20s %-10s %-10s\n", "Name", "PID", "Uptime");
    printf("%-20s %-10s %-10s\n", "----", "---", "------");
    
    uint32_t current_time = to_ms_since_boot(get_absolute_time()) / 1000;
    for (int i = 0; i < process_count; i++) {
        if (processes[i].running) {
            uint32_t uptime = current_time - processes[i].start_time;
            printf("%-20s %-10d %02lu:%02lu:%02lu\n", 
                   processes[i].name, i, 
                   uptime / 3600, (uptime % 3600) / 60, uptime % 60);
        }
    }
    printf("\n");
}

void stop_process(const char* name) {
    for (int i = 0; i < process_count; i++) {
        if (processes[i].running && strcmp(processes[i].name, name) == 0) {
            processes[i].running = false;
            multicore_reset_core1();
            printf(ANSI_GREEN "Process '%s' stopped\n" ANSI_RESET, name);
            log_message("Process stopped");
            return;
        }
    }
    printf(ANSI_RED "Process '%s' not found\n" ANSI_RESET, name);
}

// ===== NMAP - TCP PORT SCANNER =====
struct tcp_scan_state {
    ip_addr_t target_ip;
    uint16_t port;
    bool connected;
    bool finished;
};

static err_t scan_connected_callback(void *arg, struct tcp_pcb *tpcb, err_t err) {
    struct tcp_scan_state *state = (struct tcp_scan_state *)arg;
    state->connected = (err == ERR_OK);
    state->finished = true;
    tcp_abort(tpcb);
    return ERR_ABRT;
}

static void scan_error_callback(void *arg, err_t err) {
    struct tcp_scan_state *state = (struct tcp_scan_state *)arg;
    state->connected = false;
    state->finished = true;
}

bool scan_port(ip_addr_t *target, uint16_t port, uint32_t timeout_ms) {
    struct tcp_scan_state state;
    state.target_ip = *target;
    state.port = port;
    state.connected = false;
    state.finished = false;
    
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) {
        return false;
    }
    
    tcp_arg(pcb, &state);
    tcp_err(pcb, scan_error_callback);
    
    cyw43_arch_lwip_begin();
    err_t err = tcp_connect(pcb, target, port, scan_connected_callback);
    cyw43_arch_lwip_end();
    
    if (err != ERR_OK) {
        tcp_abort(pcb);
        return false;
    }
    
    // Wait for connection result with timeout
    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (!state.finished && (to_ms_since_boot(get_absolute_time()) - start < timeout_ms)) {
        sleep_ms(10);
    }
    
    if (!state.finished) {
        tcp_abort(pcb);
        return false;
    }
    
    return state.connected;
}

void nmap_app() {
    if (!wifi_connected) {
        printf(ANSI_RED "\nWiFi not connected. Cannot perform port scan.\n" ANSI_RESET);
        return;
    }
    
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║         NMAP - Port Scanner            ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    
    char *target_input = read_line("\nEnter target IP or hostname: ", true);
    if (!target_input || strlen(target_input) == 0) {
        printf(ANSI_RED "Invalid input\n" ANSI_RESET);
        return;
    }
    
    ip_addr_t target_ip;
    if (!ipaddr_aton(target_input, &target_ip)) {
        printf("Resolving hostname...\n");
        // For simplicity, require IP address
        printf(ANSI_RED "Please use IP address format (e.g., 192.168.1.1)\n" ANSI_RESET);
        return;
    }
    
    char *range_input = read_line("Port range (e.g., 1-1024 or 'common'): ", true);
    if (!range_input || strlen(range_input) == 0) {
        printf(ANSI_RED "Invalid input\n" ANSI_RESET);
        return;
    }
    
    uint16_t start_port = 1;
    uint16_t end_port = 1024;
    
    if (strcmp(range_input, "common") == 0) {
        // Scan common ports
        uint16_t common_ports[] = {21, 22, 23, 25, 53, 80, 110, 143, 443, 445, 3306, 3389, 5432, 8080, 8443};
        int num_common = sizeof(common_ports) / sizeof(common_ports[0]);
        
        printf("\nScanning %d common ports on %s...\n\n", num_common, target_input);
        printf(ANSI_BOLD "PORT     STATE      SERVICE\n" ANSI_RESET);
        printf("----     -----      -------\n");
        
        int open_count = 0;
        for (int i = 0; i < num_common; i++) {
            printf("Scanning port %d...\r", common_ports[i]);
            fflush(stdout);
            
            bool is_open = scan_port(&target_ip, common_ports[i], 1000);
            if (is_open) {
                const char *service = "unknown";
                switch (common_ports[i]) {
                    case 21: service = "ftp"; break;
                    case 22: service = "ssh"; break;
                    case 23: service = "telnet"; break;
                    case 25: service = "smtp"; break;
                    case 53: service = "dns"; break;
                    case 80: service = "http"; break;
                    case 110: service = "pop3"; break;
                    case 143: service = "imap"; break;
                    case 443: service = "https"; break;
                    case 445: service = "smb"; break;
                    case 3306: service = "mysql"; break;
                    case 3389: service = "rdp"; break;
                    case 5432: service = "postgresql"; break;
                    case 8080: service = "http-alt"; break;
                    case 8443: service = "https-alt"; break;
                }
                printf(ANSI_GREEN "%-8d %-10s %s\n" ANSI_RESET, common_ports[i], "open", service);
                open_count++;
            }
        }
        
        printf("\n" ANSI_BOLD "Scan complete: %d open ports found\n" ANSI_RESET, open_count);
        
    } else {
        // Parse range
        char *dash = strchr(range_input, '-');
        if (dash) {
            *dash = '\0';
            start_port = atoi(range_input);
            end_port = atoi(dash + 1);
        } else {
            start_port = end_port = atoi(range_input);
        }
        
        if (start_port < 1 || start_port > end_port) {
            printf(ANSI_RED "Invalid port range\n" ANSI_RESET);
            return;
        }
        
        printf("\nScanning ports %d-%d on %s...\n\n", start_port, end_port, target_input);
        printf(ANSI_BOLD "PORT     STATE\n" ANSI_RESET);
        printf("----     -----\n");
        
        int open_count = 0;
        for (uint16_t port = start_port; port <= end_port; port++) {
            printf("Scanning port %d...\r", port);
            fflush(stdout);
            
            bool is_open = scan_port(&target_ip, port, 500);
            if (is_open) {
                printf(ANSI_GREEN "%-8d open\n" ANSI_RESET, port);
                open_count++;
            }
        }
        
        printf("\n" ANSI_BOLD "Scan complete: %d open ports found\n" ANSI_RESET, open_count);
    }
    
    read_line("\nPress Enter to continue...", true);
}

// ===== ASCII ART CONVERTER =====
void ascii_converter() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║       ASCII Art Text Converter         ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    
    char *input_text = read_line("\nEnter text to convert: ", true);
    if (!input_text || strlen(input_text) == 0) {
        printf(ANSI_RED "No text entered\n" ANSI_RESET);
        return;
    }
    
    // Copy to local buffer to prevent it being overwritten
    char text[64];
    strncpy(text, input_text, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    
    if (strlen(text) > 20) {
        printf(ANSI_YELLOW "Warning: Text too long. Truncating to 20 characters.\n" ANSI_RESET);
        text[20] = '\0';
    }
    
    // Convert text to uppercase for consistency
    for (int i = 0; text[i]; i++) {
        text[i] = toupper(text[i]);
    }
    
    printf("\n");
    
    // Slash style ASCII art - 5 lines high, using / and \ characters
    for (int line = 0; line < 5; line++) {
        for (int i = 0; text[i]; i++) {
            char c = text[i];
            
            if (c == ' ') {
                printf("    ");
            } else if (c >= 'A' && c <= 'Z') {
                // Letter definitions using / and \ characters
                switch (line) {
                    case 0: // Top line
                        if (c == 'A') printf(" /\\ ");
                        else if (c == 'B') printf("/==\\");
                        else if (c == 'C') printf(" /==");
                        else if (c == 'D') printf("/==\\");
                        else if (c == 'E') printf("/===");
                        else if (c == 'F') printf("/===");
                        else if (c == 'G') printf(" /==");
                        else if (c == 'H') printf("/  \\");
                        else if (c == 'I') printf("===");
                        else if (c == 'J') printf("  /");
                        else if (c == 'K') printf("/  \\");
                        else if (c == 'L') printf("/   ");
                        else if (c == 'M') printf("/\\  /\\");
                        else if (c == 'N') printf("/\\  \\");
                        else if (c == 'O') printf(" /\\ ");
                        else if (c == 'P') printf("/==\\");
                        else if (c == 'Q') printf(" /\\ ");
                        else if (c == 'R') printf("/==\\");
                        else if (c == 'S') printf(" /==");
                        else if (c == 'T') printf("===");
                        else if (c == 'U') printf("\\  /");
                        else if (c == 'V') printf("\\  /");
                        else if (c == 'W') printf("\\    /");
                        else if (c == 'X') printf("\\  /");
                        else if (c == 'Y') printf("\\ /");
                        else if (c == 'Z') printf("===");
                        else printf("    ");
                        break;
                        
                    case 1: // Second line
                        if (c == 'A') printf("/  \\");
                        else if (c == 'B') printf("|-- ");
                        else if (c == 'C') printf("|   ");
                        else if (c == 'D') printf("|  \\");
                        else if (c == 'E') printf("|-- ");
                        else if (c == 'F') printf("|-- ");
                        else if (c == 'G') printf("|   ");
                        else if (c == 'H') printf("|--|");
                        else if (c == 'I') printf(" | ");
                        else if (c == 'J') printf("  |");
                        else if (c == 'K') printf("|-/ ");
                        else if (c == 'L') printf("|   ");
                        else if (c == 'M') printf("| \\/ |");
                        else if (c == 'N') printf("| \\ |");
                        else if (c == 'O') printf("|  |");
                        else if (c == 'P') printf("|--/");
                        else if (c == 'Q') printf("|  |");
                        else if (c == 'R') printf("|--/");
                        else if (c == 'S') printf("\\__ ");
                        else if (c == 'T') printf(" | ");
                        else if (c == 'U') printf("|  |");
                        else if (c == 'V') printf(" \\/");
                        else if (c == 'W') printf(" \\  / ");
                        else if (c == 'X') printf(" \\/ ");
                        else if (c == 'Y') printf(" | ");
                        else if (c == 'Z') printf(" / ");
                        else printf("    ");
                        break;
                        
                    case 2: // Middle line
                        if (c == 'A') printf("/==\\");
                        else if (c == 'B') printf("|==\\");
                        else if (c == 'C') printf("|   ");
                        else if (c == 'D') printf("|  |");
                        else if (c == 'E') printf("|-- ");
                        else if (c == 'F') printf("|   ");
                        else if (c == 'G') printf("| -+");
                        else if (c == 'H') printf("|  |");
                        else if (c == 'I') printf(" | ");
                        else if (c == 'J') printf("  |");
                        else if (c == 'K') printf("|-\\ ");
                        else if (c == 'L') printf("|   ");
                        else if (c == 'M') printf("|    |");
                        else if (c == 'N') printf("|  \\|");
                        else if (c == 'O') printf("|  |");
                        else if (c == 'P') printf("|   ");
                        else if (c == 'Q') printf("| \\|");
                        else if (c == 'R') printf("|-\\ ");
                        else if (c == 'S') printf(" __/");
                        else if (c == 'T') printf(" | ");
                        else if (c == 'U') printf("|  |");
                        else if (c == 'V') printf(" /\\");
                        else if (c == 'W') printf("  \\/  ");
                        else if (c == 'X') printf(" /\\ ");
                        else if (c == 'Y') printf(" | ");
                        else if (c == 'Z') printf("/  ");
                        else printf("    ");
                        break;
                        
                    case 3: // Fourth line
                        if (c == 'A') printf("|  |");
                        else if (c == 'B') printf("|  |");
                        else if (c == 'C') printf("|   ");
                        else if (c == 'D') printf("|  /");
                        else if (c == 'E') printf("|   ");
                        else if (c == 'F') printf("|   ");
                        else if (c == 'G') printf("|  |");
                        else if (c == 'H') printf("|  |");
                        else if (c == 'I') printf(" | ");
                        else if (c == 'J') printf("\\ |");
                        else if (c == 'K') printf("| \\");
                        else if (c == 'L') printf("|   ");
                        else if (c == 'M') printf("|    |");
                        else if (c == 'N') printf("|   |");
                        else if (c == 'O') printf("|  |");
                        else if (c == 'P') printf("|   ");
                        else if (c == 'Q') printf(" \\|\\");
                        else if (c == 'R') printf("| \\");
                        else if (c == 'S') printf("\\  \\");
                        else if (c == 'T') printf(" | ");
                        else if (c == 'U') printf("|  |");
                        else if (c == 'V') printf("/  \\");
                        else if (c == 'W') printf(" /  \\ ");
                        else if (c == 'X') printf("/  \\");
                        else if (c == 'Y') printf(" | ");
                        else if (c == 'Z') printf("/   ");
                        else printf("    ");
                        break;
                        
                    case 4: // Bottom line
                        if (c == 'A') printf("|  |");
                        else if (c == 'B') printf("\\==/");
                        else if (c == 'C') printf(" \\==");
                        else if (c == 'D') printf("\\==/");
                        else if (c == 'E') printf("\\===");
                        else if (c == 'F') printf("|   ");
                        else if (c == 'G') printf(" \\==");
                        else if (c == 'H') printf("|  |");
                        else if (c == 'I') printf("===");
                        else if (c == 'J') printf(" \\/ ");
                        else if (c == 'K') printf("|  \\");
                        else if (c == 'L') printf("\\___");
                        else if (c == 'M') printf("|    |");
                        else if (c == 'N') printf("|   |");
                        else if (c == 'O') printf(" \\/ ");
                        else if (c == 'P') printf("|   ");
                        else if (c == 'Q') printf("  \\_\\");
                        else if (c == 'R') printf("|  \\");
                        else if (c == 'S') printf("\\==/");
                        else if (c == 'T') printf(" | ");
                        else if (c == 'U') printf(" \\/ ");
                        else if (c == 'V') printf("|  |");
                        else if (c == 'W') printf("/    \\");
                        else if (c == 'X') printf("|  |");
                        else if (c == 'Y') printf(" | ");
                        else if (c == 'Z') printf("===");
                        else printf("    ");
                        break;
                }
                printf(" ");
            } else if (c >= '0' && c <= '9') {
                // Number support
                switch (line) {
                    case 0: printf(" /==\\"); break;
                    case 1: printf("|  %c|", c); break;
                    case 2: printf("|  %c|", c); break;
                    case 3: printf("|  %c|", c); break;
                    case 4: printf(" \\==/"); break;
                }
                printf(" ");
            }
        }
        printf("\n");
    }
    
    read_line("\nPress Enter to continue...", true);
}

// ===== TETRIS GAME =====
static const int tetris_shapes[7][4][4] = {
    // I piece
    {{0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0}},
    // O piece
    {{0,0,0,0}, {0,1,1,0}, {0,1,1,0}, {0,0,0,0}},
    // T piece
    {{0,0,0,0}, {1,1,1,0}, {0,1,0,0}, {0,0,0,0}},
    // S piece
    {{0,0,0,0}, {0,1,1,0}, {1,1,0,0}, {0,0,0,0}},
    // Z piece
    {{0,0,0,0}, {1,1,0,0}, {0,1,1,0}, {0,0,0,0}},
    // J piece
    {{0,0,0,0}, {1,1,1,0}, {0,0,1,0}, {0,0,0,0}},
    // L piece
    {{0,0,0,0}, {1,1,1,0}, {1,0,0,0}, {0,0,0,0}}
};

void rotate_piece(TetrisPiece *piece) {
    int temp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            temp[i][j] = piece->shape[3-j][i];
        }
    }
    memcpy(piece->shape, temp, sizeof(temp));
}

bool check_collision(int board[TETRIS_HEIGHT][TETRIS_WIDTH], TetrisPiece *piece, int dx, int dy) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (piece->shape[i][j]) {
                int new_x = piece->x + j + dx;
                int new_y = piece->y + i + dy;
                
                if (new_x < 0 || new_x >= TETRIS_WIDTH || new_y >= TETRIS_HEIGHT) {
                    return true;
                }
                if (new_y >= 0 && board[new_y][new_x]) {
                    return true;
                }
            }
        }
    }
    return false;
}

void merge_piece(int board[TETRIS_HEIGHT][TETRIS_WIDTH], TetrisPiece *piece) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            if (piece->shape[i][j]) {
                int x = piece->x + j;
                int y = piece->y + i;
                if (y >= 0 && y < TETRIS_HEIGHT && x >= 0 && x < TETRIS_WIDTH) {
                    board[y][x] = piece->type + 1;
                }
            }
        }
    }
}

int clear_lines(int board[TETRIS_HEIGHT][TETRIS_WIDTH]) {
    int lines_cleared = 0;
    for (int i = TETRIS_HEIGHT - 1; i >= 0; i--) {
        bool full = true;
        for (int j = 0; j < TETRIS_WIDTH; j++) {
            if (board[i][j] == 0) {
                full = false;
                break;
            }
        }
        if (full) {
            lines_cleared++;
            for (int k = i; k > 0; k--) {
                memcpy(board[k], board[k-1], sizeof(board[0]));
            }
            memset(board[0], 0, sizeof(board[0]));
            i++; // Check this line again
        }
    }
    return lines_cleared;
}

void draw_tetris_board(int board[TETRIS_HEIGHT][TETRIS_WIDTH], TetrisPiece *piece, int score, int level) {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════╗\n");
    printf("║        TETRIS          ║\n");
    printf("╚════════════════════════╝\n" ANSI_RESET);
    printf("Score: %d  Level: %d\n\n", score, level);
    
    // Create temporary board with current piece
    int display[TETRIS_HEIGHT][TETRIS_WIDTH];
    memcpy(display, board, sizeof(display));
    
    // Add current piece to display
    if (piece) {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                if (piece->shape[i][j]) {
                    int x = piece->x + j;
                    int y = piece->y + i;
                    if (y >= 0 && y < TETRIS_HEIGHT && x >= 0 && x < TETRIS_WIDTH) {
                        display[y][x] = piece->type + 1;
                    }
                }
            }
        }
    }
    
    // Draw board
    printf("┌");
    for (int i = 0; i < TETRIS_WIDTH; i++) printf("──");
    printf("┐\n");
    
    for (int i = 0; i < TETRIS_HEIGHT; i++) {
        printf("│");
        for (int j = 0; j < TETRIS_WIDTH; j++) {
            if (display[i][j] == 0) {
                printf("  ");
            } else {
                const char *colors[] = {ANSI_CYAN, ANSI_YELLOW, ANSI_MAGENTA, ANSI_GREEN, ANSI_RED, ANSI_BLUE, ANSI_RESET};
                printf("%s▓▓" ANSI_RESET, colors[display[i][j] - 1]);
            }
        }
        printf("│\n");
    }
    
    printf("└");
    for (int i = 0; i < TETRIS_WIDTH; i++) printf("──");
    printf("┘\n");
    
    printf("\nControls: A/D=Move  W=Rotate  S=Drop  Q=Quit\n");
}

void tetris_game() {
    int board[TETRIS_HEIGHT][TETRIS_WIDTH] = {0};
    int score = 0;
    int level = 1;
    int lines = 0;
    bool game_over = false;
    
    TetrisPiece current_piece;
    current_piece.type = rand() % 7;
    memcpy(current_piece.shape, tetris_shapes[current_piece.type], sizeof(current_piece.shape));
    current_piece.x = TETRIS_WIDTH / 2 - 2;
    current_piece.y = 0;
    
    uint32_t last_drop = to_ms_since_boot(get_absolute_time());
    uint32_t drop_interval = 1000 - (level - 1) * 100;
    if (drop_interval < 100) drop_interval = 100;
    
    while (!game_over) {
        draw_tetris_board(board, &current_piece, score, level);
        
        // Check for input
        int c = getchar_timeout_us(50000);
        
        if (c == 'q' || c == 'Q') {
            break;
        } else if (c == 'a' || c == 'A') {
            if (!check_collision(board, &current_piece, -1, 0)) {
                current_piece.x--;
            }
        } else if (c == 'd' || c == 'D') {
            if (!check_collision(board, &current_piece, 1, 0)) {
                current_piece.x++;
            }
        } else if (c == 'w' || c == 'W') {
            TetrisPiece test_piece = current_piece;
            rotate_piece(&test_piece);
            if (!check_collision(board, &test_piece, 0, 0)) {
                current_piece = test_piece;
            }
        } else if (c == 's' || c == 'S') {
            while (!check_collision(board, &current_piece, 0, 1)) {
                current_piece.y++;
            }
        }
        
        // Auto drop
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_drop >= drop_interval) {
            if (!check_collision(board, &current_piece, 0, 1)) {
                current_piece.y++;
            } else {
                // Piece landed
                merge_piece(board, &current_piece);
                int cleared = clear_lines(board);
                if (cleared > 0) {
                    lines += cleared;
                    score += cleared * cleared * 100;
                    level = 1 + lines / 10;
                    drop_interval = 1000 - (level - 1) * 100;
                    if (drop_interval < 100) drop_interval = 100;
                }
                
                // New piece
                current_piece.type = rand() % 7;
                memcpy(current_piece.shape, tetris_shapes[current_piece.type], sizeof(current_piece.shape));
                current_piece.x = TETRIS_WIDTH / 2 - 2;
                current_piece.y = 0;
                
                if (check_collision(board, &current_piece, 0, 0)) {
                    game_over = true;
                }
            }
            last_drop = now;
        }
        
        sleep_ms(50);
    }
    
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_RED "\n╔════════════════════════╗\n");
    printf("║      GAME OVER!        ║\n");
    printf("╚════════════════════════╝\n" ANSI_RESET);
    printf("\nFinal Score: %d\n", score);
    printf("Level Reached: %d\n", level);
    printf("Lines Cleared: %d\n\n", lines);
    
    read_line("Press Enter to continue...", true);
}

// ===== SNAKE GAME =====
void draw_snake_board(int board[SNAKE_HEIGHT][SNAKE_WIDTH], int score) {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_GREEN "╔════════════════════════╗\n");
    printf("║         SNAKE          ║\n");
    printf("╚════════════════════════╝\n" ANSI_RESET);
    printf("Score: %d\n\n", score);
    
    printf("┌");
    for (int i = 0; i < SNAKE_WIDTH; i++) printf("─");
    printf("┐\n");
    
    for (int i = 0; i < SNAKE_HEIGHT; i++) {
        printf("│");
        for (int j = 0; j < SNAKE_WIDTH; j++) {
            if (board[i][j] == 0) {
                printf(" ");
            } else if (board[i][j] == 1) {
                printf(ANSI_GREEN "●" ANSI_RESET);
            } else if (board[i][j] == 2) {
                printf(ANSI_RED "◆" ANSI_RESET);
            }
        }
        printf("│\n");
    }
    
    printf("└");
    for (int i = 0; i < SNAKE_WIDTH; i++) printf("─");
    printf("┘\n");
    
    printf("\nControls: W/A/S/D=Move  Q=Quit\n");
}

void snake_game() {
    int board[SNAKE_HEIGHT][SNAKE_WIDTH] = {0};
    SnakeSegment snake[SNAKE_MAX_LENGTH];
    int snake_length = 3;
    int dx = 1, dy = 0;
    int score = 0;
    bool game_over = false;
    
    // Initialize snake
    snake[0].x = SNAKE_WIDTH / 2;
    snake[0].y = SNAKE_HEIGHT / 2;
    snake[1].x = snake[0].x - 1;
    snake[1].y = snake[0].y;
    snake[2].x = snake[1].x - 1;
    snake[2].y = snake[1].y;
    
    // Place first food
    int food_x = rand() % SNAKE_WIDTH;
    int food_y = rand() % SNAKE_HEIGHT;
    
    uint32_t last_move = to_ms_since_boot(get_absolute_time());
    uint32_t move_interval = 200;
    
    char pending_direction = 'd';
    
    while (!game_over) {
        // Update board
        memset(board, 0, sizeof(board));
        for (int i = 0; i < snake_length; i++) {
            board[snake[i].y][snake[i].x] = 1;
        }
        board[food_y][food_x] = 2;
        
        draw_snake_board(board, score);
        
        // Check for input
        int c = getchar_timeout_us(50000);
        if (c != PICO_ERROR_TIMEOUT) {
            if ((c == 'w' || c == 'W') && dy != 1) {
                pending_direction = 'w';
            } else if ((c == 's' || c == 'S') && dy != -1) {
                pending_direction = 's';
            } else if ((c == 'a' || c == 'A') && dx != 1) {
                pending_direction = 'a';
            } else if ((c == 'd' || c == 'D') && dx != -1) {
                pending_direction = 'd';
            } else if (c == 'q' || c == 'Q') {
                break;
            }
        }
        
        // Move snake
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_move >= move_interval) {
            // Update direction
            if (pending_direction == 'w') {
                dx = 0; dy = -1;
            } else if (pending_direction == 's') {
                dx = 0; dy = 1;
            } else if (pending_direction == 'a') {
                dx = -1; dy = 0;
            } else if (pending_direction == 'd') {
                dx = 1; dy = 0;
            }
            
            // Calculate new head position
            int new_x = snake[0].x + dx;
            int new_y = snake[0].y + dy;
            
            // Check wall collision
            if (new_x < 0 || new_x >= SNAKE_WIDTH || new_y < 0 || new_y >= SNAKE_HEIGHT) {
                game_over = true;
                break;
            }
            
            // Check self collision
            for (int i = 0; i < snake_length; i++) {
                if (snake[i].x == new_x && snake[i].y == new_y) {
                    game_over = true;
                    break;
                }
            }
            
            if (game_over) break;
            
            // Check food collision
            bool ate_food = (new_x == food_x && new_y == food_y);
            
            // Move snake
            if (!ate_food) {
                for (int i = snake_length - 1; i > 0; i--) {
                    snake[i] = snake[i - 1];
                }
            } else {
                // Grow snake
                if (snake_length < SNAKE_MAX_LENGTH) {
                    for (int i = snake_length; i > 0; i--) {
                        snake[i] = snake[i - 1];
                    }
                    snake_length++;
                    score += 10;
                    
                    // Speed up
                    move_interval -= 5;
                    if (move_interval < 50) move_interval = 50;
                }
                
                // Place new food
                do {
                    food_x = rand() % SNAKE_WIDTH;
                    food_y = rand() % SNAKE_HEIGHT;
                    bool on_snake = false;
                    for (int i = 0; i < snake_length; i++) {
                        if (snake[i].x == food_x && snake[i].y == food_y) {
                            on_snake = true;
                            break;
                        }
                    }
                    if (!on_snake) break;
                } while (true);
            }
            
            snake[0].x = new_x;
            snake[0].y = new_y;
            
            last_move = now;
        }
        
        sleep_ms(50);
    }
    
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_RED "\n╔════════════════════════╗\n");
    printf("║      GAME OVER!        ║\n");
    printf("╚════════════════════════╝\n" ANSI_RESET);
    printf("\nFinal Score: %d\n", score);
    printf("Snake Length: %d\n\n", snake_length);
    
    read_line("Press Enter to continue...", true);
}

// ===== ORIGINAL APPS (ABBREVIATED) =====

void show_help() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔═════════ PICO OS v2.0 COMMANDS ══════════╗\n" ANSI_RESET);
    printf("\n");
    
    // Compact single-column format that works on all screen sizes
    printf(ANSI_BOLD "SYSTEM:\n" ANSI_RESET);
    printf("  help, neofetch, sysinfo, clear, reboot\n");
    printf("  time, viewlog, showram, setting\n");
    printf("\n");
    
    printf(ANSI_BOLD "FILES:\n" ANSI_RESET);
    printf("  ls, cat <file>, nano <file>, make <file>\n");
    printf("  delete <file>, showspace\n");
    printf("\n");
    
    printf(ANSI_BOLD "NETWORK:\n" ANSI_RESET);
    printf("  wifi, ipa, ping <host>, nmap\n");
    printf("\n");
    
    printf(ANSI_BOLD ANSI_GREEN "WEB SERVER:\n" ANSI_RESET);
    printf("  localhost, stopweb, createweb\n");
    printf("\n");
    
    printf(ANSI_BOLD "APPS:\n" ANSI_RESET);
    printf("  timer, todo, ascii, tetris, snake\n");
    printf("\n");
    
    printf(ANSI_BOLD "PROCESS:\n" ANSI_RESET);
    printf("  ps, stop <name>\n");
    printf("\n");
    
    printf(ANSI_CYAN "╚════════════════════════════════════════════╝\n" ANSI_RESET);
    printf("Tip: Type " ANSI_BOLD "sysinfo" ANSI_RESET " for system details\n");
}

void neofetch() {
    printf(ANSI_CLEAR_SCREEN);
    printf("\n");
    printf(ANSI_RED "  .~~.   .~~.\n");
    printf(" '. \\ ' ' / .'\n");
    printf("  .~ .~~~..~.\n");
    printf(" : .~.'~'.~. :\n");
    printf("~ (   ) (   ) ~\n");
    printf("( : '~'.~.'~' : )\n");
    printf(" ~ .~ (   ) ~. ~\n");
    printf("  (  : '~' :  )\n");
    printf("   '~ .~~~. ~'\n");
    printf("       '~'\n" ANSI_RESET);
    printf("\n");
    
    time_t now = get_current_time();
    uint32_t uptime_sec = to_ms_since_boot(get_absolute_time()) / 1000;
    
    printf(ANSI_BOLD ANSI_BLUE "pico@os\n" ANSI_RESET);
    printf("-------\n");
    printf(ANSI_BOLD "OS:" ANSI_RESET " Pico OS v2.0\n");
    printf(ANSI_BOLD "Host:" ANSI_RESET " Raspberry Pi Pico 2 W\n");
    printf(ANSI_BOLD "Kernel:" ANSI_RESET " RP2350 (Dual Cortex-M33)\n");
    printf(ANSI_BOLD "Uptime:" ANSI_RESET " %lu days, %02lu:%02lu:%02lu\n",
           uptime_sec / 86400, (uptime_sec % 86400) / 3600,
           (uptime_sec % 3600) / 60, uptime_sec % 60);
    printf(ANSI_BOLD "Shell:" ANSI_RESET " PicoShell\n");
    
    if (wifi_connected) {
        printf(ANSI_BOLD "WiFi:" ANSI_RESET " " ANSI_GREEN "Connected" ANSI_RESET " (%s)\n", wifi_ssid);
    } else {
        printf(ANSI_BOLD "WiFi:" ANSI_RESET " " ANSI_RED "Disconnected" ANSI_RESET "\n");
    }
    
    if (now != 0) {
        struct tm *t = localtime(&now);
        printf(ANSI_BOLD "Time:" ANSI_RESET " %04d-%02d-%02d %02d:%02d:%02d %s\n",
               t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
               t->tm_hour, t->tm_min, t->tm_sec, timezone_str);
    }
    
    printf("\n");
    printf(ANSI_RED "███" ANSI_RESET ANSI_YELLOW "███" ANSI_RESET ANSI_GREEN "███" ANSI_RESET 
           ANSI_CYAN "███" ANSI_RESET ANSI_BLUE "███" ANSI_RESET ANSI_MAGENTA "███" ANSI_RESET "\n");
    printf("\n");
}

void show_system_info() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║         System Information             ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    printf("\n");
    
    uint32_t uptime_sec = to_ms_since_boot(get_absolute_time()) / 1000;
    printf(ANSI_BOLD "System Uptime:\n" ANSI_RESET);
    printf("  %lu days, %02lu:%02lu:%02lu\n\n",
           uptime_sec / 86400, (uptime_sec % 86400) / 3600,
           (uptime_sec % 3600) / 60, uptime_sec % 60);
    
    printf(ANSI_BOLD "Hardware:\n" ANSI_RESET);
    printf("  Chip: RP2350 (Raspberry Pi Pico 2 W)\n");
    printf("  Cores: Dual Cortex-M33 @ 150MHz\n");
    printf("  RAM: 520KB SRAM\n");
    printf("  Flash: 2MB\n");
    printf("  Wireless: CYW43439 (WiFi + Bluetooth)\n\n");
    
    printf(ANSI_BOLD "Network Status:\n" ANSI_RESET);
    if (wifi_connected) {
        printf("  WiFi: " ANSI_GREEN "Connected" ANSI_RESET "\n");
        printf("  SSID: %s\n", wifi_ssid);
        
        // Show IP if available
        const ip4_addr_t *ip = netif_ip4_addr(netif_default);
        if (ip) {
            printf("  IP: %s\n", ip4addr_ntoa(ip));
        }
    } else {
        printf("  WiFi: " ANSI_RED "Disconnected" ANSI_RESET "\n");
    }
    
    printf("\n");
}

void timer_app() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║            Timer Application           ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    
    char *input = read_line("\nEnter duration in seconds: ", true);
    if (!input || strlen(input) == 0) {
        printf(ANSI_RED "Invalid input\n" ANSI_RESET);
        return;
    }
    
    int duration = atoi(input);
    
    if (duration <= 0 || duration > 86400) { // Max 24 hours
        printf(ANSI_RED "Invalid duration (must be 1-86400 seconds)\n" ANSI_RESET);
        return;
    }
    
    printf("\nTimer started for %d seconds...\n", duration);
    printf("Press any key to cancel\n\n");
    
    for (int i = duration; i > 0; i--) {
        printf("\r" ANSI_BOLD ANSI_YELLOW "Time remaining: %02d:%02d" ANSI_RESET, i / 60, i % 60);
        fflush(stdout);
        
        for (int j = 0; j < 100; j++) {
            int c = getchar_timeout_us(10000);
            if (c != PICO_ERROR_TIMEOUT) {
                printf("\n\n" ANSI_YELLOW "Timer cancelled!\n" ANSI_RESET);
                return;
            }
        }
    }
    
    printf("\n\n" ANSI_GREEN ANSI_BOLD "⏰ TIME'S UP! ⏰\n" ANSI_RESET);
    for (int i = 0; i < 5; i++) {
        printf("\a");
        sleep_ms(500);
    }
    
    read_line("\nPress Enter to continue...", true);
}

void todo_app() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║          Todo List Manager             ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    
    while (true) {
        printf("\n" ANSI_BOLD "Current Todos:\n" ANSI_RESET);
        bool has_todos = false;
        for (int i = 0; i < 2; i++) {
            if (todos[i].active) {
                has_todos = true;
                printf("%d. [%c] %s\n", i + 1,
                       todos[i].completed ? 'X' : ' ',
                       todos[i].text);
            }
        }
        if (!has_todos) {
            printf("  (No todos yet)\n");
        }
        
        printf("\nOptions:\n");
        printf("1. Add todo\n");
        printf("2. Complete todo\n");
        printf("3. Delete todo\n");
        printf("4. Exit\n");
        
        char *choice = read_line("\nChoice: ", true);
        
        if (strcmp(choice, "1") == 0) {
            for (int i = 0; i < 2; i++) {
                if (!todos[i].active) {
                    char *text = read_line("Enter todo: ", true);
                    if (!text || strlen(text) == 0) {
                        printf(ANSI_YELLOW "Todo text cannot be empty\n" ANSI_RESET);
                        break;
                    }
                    strncpy(todos[i].text, text, sizeof(todos[i].text) - 1);
                    todos[i].text[sizeof(todos[i].text) - 1] = '\0';
                    todos[i].active = true;
                    todos[i].completed = false;
                    printf(ANSI_GREEN "Todo added!\n" ANSI_RESET);
                    break;
                }
            }
        } else if (strcmp(choice, "2") == 0) {
            char *num = read_line("Todo number to complete: ", true);
            if (!num) continue;
            int idx = atoi(num) - 1;
            if (idx >= 0 && idx < 2 && todos[idx].active) {
                todos[idx].completed = !todos[idx].completed;
                printf(ANSI_GREEN "Todo toggled!\n" ANSI_RESET);
            } else {
                printf(ANSI_YELLOW "Invalid todo number\n" ANSI_RESET);
            }
        } else if (strcmp(choice, "3") == 0) {
            char *num = read_line("Todo number to delete: ", true);
            if (!num) continue;
            int idx = atoi(num) - 1;
            if (idx >= 0 && idx < 2 && todos[idx].active) {
                todos[idx].active = false;
                todos[idx].text[0] = '\0';
                printf(ANSI_GREEN "Todo deleted!\n" ANSI_RESET);
            } else {
                printf(ANSI_YELLOW "Invalid todo number\n" ANSI_RESET);
            }
        } else if (strcmp(choice, "4") == 0) {
            break;
        }
        
        printf(ANSI_CLEAR_SCREEN);
        printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
        printf("║          Todo List Manager             ║\n");
        printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    }
}

void list_files() {
    printf("\n" ANSI_BOLD "Files:\n" ANSI_RESET);
    lfs_dir_t dir;
    struct lfs_info info;
    
    int err = lfs_dir_open(&lfs, &dir, "/");
    if (err) {
        printf(ANSI_RED "Error opening directory\n" ANSI_RESET);
        return;
    }
    
    bool found_files = false;
    while (true) {
        int res = lfs_dir_read(&lfs, &dir, &info);
        if (res < 0) break;
        if (res == 0) break;
        
        if (strcmp(info.name, ".") != 0 && strcmp(info.name, "..") != 0) {
            found_files = true;
            printf("  %s (%ld bytes)\n", info.name, info.size);
        }
    }
    
    if (!found_files) {
        printf("  (No files)\n");
    }
    
    lfs_dir_close(&lfs, &dir);
    printf("\n");
}

void view_file(const char* filename) {
    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, filename, LFS_O_RDONLY);
    if (err < 0) {
        printf(ANSI_RED "Error: File not found\n" ANSI_RESET);
        return;
    }
    
    printf("\n" ANSI_BOLD "Contents of %s:\n" ANSI_RESET, filename);
    printf("─────────────────────────────────────\n");
    
    char buffer[256];
    int read_size;
    while ((read_size = lfs_file_read(&lfs, &file, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[read_size] = '\0';
        printf("%s", buffer);
    }
    
    printf("\n─────────────────────────────────────\n\n");
    lfs_file_close(&lfs, &file);
}

void nano_editor(const char* filename) {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD "Nano Editor - %s\n" ANSI_RESET, filename);
    printf("─────────────────────────────────────\n");
    printf("Enter text (Ctrl+D on new line to save and exit):\n\n");
    
    char buffer[1024] = {0};
    size_t idx = 0;
    
    while (idx < sizeof(buffer) - 1) {
        int c = getchar();
        if (c == 4) break; // Ctrl+D
        if (c == '\r') {
            buffer[idx++] = '\n';
            printf("\r\n");
        } else if (c == 127 || c == 8) {
            if (idx > 0) {
                idx--;
                printf("\b \b");
            }
        } else if (c >= 32 && c < 127) {
            buffer[idx++] = c;
            putchar(c);
        }
        fflush(stdout);
    }
    
    lfs_file_t file;
    int err = lfs_file_open(&lfs, &file, filename, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (err < 0) {
        printf("\n" ANSI_RED "Error: Could not save file\n" ANSI_RESET);
        return;
    }
    
    lfs_file_write(&lfs, &file, buffer, idx);
    lfs_file_close(&lfs, &file);
    
    printf("\n\n" ANSI_GREEN "File saved successfully!\n" ANSI_RESET);
}

void delete_file(const char* filename) {
    int err = lfs_remove(&lfs, filename);
    if (err < 0) {
        printf(ANSI_RED "Error: Could not delete file\n" ANSI_RESET);
    } else {
        printf(ANSI_GREEN "File deleted successfully\n" ANSI_RESET);
    }
}

void show_storage_info() {
    printf("\n" ANSI_BOLD "Storage Information:\n" ANSI_RESET);
    printf("  Total: 512 KB\n");
    
    lfs_ssize_t blocks_used = lfs_fs_size(&lfs);
    if (blocks_used >= 0) {
        uint32_t bytes_used = blocks_used * LFS_BLOCK_SIZE;
        printf("  Used: %lu KB\n", bytes_used / 1024);
        printf("  Free: %lu KB\n", (512 * 1024 - bytes_used) / 1024);
    }
    printf("\n");
}

void show_ip() {
    if (!wifi_connected) {
        printf(ANSI_YELLOW "WiFi not connected\n" ANSI_RESET);
        return;
    }
    
    const ip4_addr_t *ip = netif_ip4_addr(netif_default);
    if (ip) {
        printf("\n" ANSI_BOLD "Network Information:\n" ANSI_RESET);
        printf("  IP Address: %s\n", ip4addr_ntoa(ip));
        printf("  Netmask: %s\n", ip4addr_ntoa(netif_ip4_netmask(netif_default)));
        printf("  Gateway: %s\n", ip4addr_ntoa(netif_ip4_gw(netif_default)));
        printf("\n");
    }
}

// Ping state structure
struct ping_state {
    struct raw_pcb *pcb;
    ip_addr_t target_ip;
    uint16_t seq_num;
    bool received;
    uint32_t sent_time;
    uint32_t rtt_ms;
};

static struct ping_state ping_data;

// ICMP echo receive callback
static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr) {
    struct ping_state *state = (struct ping_state *)arg;
    
    if (p->tot_len >= (PBUF_IP_HLEN + sizeof(struct icmp_echo_hdr))) {
        struct icmp_echo_hdr *iecho = (struct icmp_echo_hdr *)((u8_t *)p->payload + PBUF_IP_HLEN);
        
        if ((iecho->type == ICMP_ER) && 
            (iecho->id == 0xABCD) && 
            (ntohs(iecho->seqno) == state->seq_num)) {
            
            state->received = true;
            state->rtt_ms = to_ms_since_boot(get_absolute_time()) - state->sent_time;
            pbuf_free(p);
            return 1; // Packet consumed
        }
    }
    
    return 0; // Packet not consumed
}

// Send ICMP echo request
static void ping_send(struct ping_state *state) {
    struct pbuf *p;
    struct icmp_echo_hdr *iecho;
    size_t ping_size = sizeof(struct icmp_echo_hdr) + 32; // 32 bytes of data
    
    p = pbuf_alloc(PBUF_IP, (u16_t)ping_size, PBUF_RAM);
    if (!p) {
        return;
    }
    
    if ((p->len == p->tot_len) && (p->next == NULL)) {
        iecho = (struct icmp_echo_hdr *)p->payload;
        
        ICMPH_TYPE_SET(iecho, ICMP_ECHO);
        ICMPH_CODE_SET(iecho, 0);
        iecho->chksum = 0;
        iecho->id = 0xABCD;
        iecho->seqno = htons(state->seq_num);
        
        // Fill data with pattern
        u8_t *data = (u8_t *)iecho + sizeof(struct icmp_echo_hdr);
        for (int i = 0; i < 32; i++) {
            data[i] = 0x20 + i;
        }
        
        iecho->chksum = inet_chksum(iecho, ping_size);
        
        state->sent_time = to_ms_since_boot(get_absolute_time());
        state->received = false;
        
        cyw43_arch_lwip_begin();
        raw_sendto(state->pcb, p, &state->target_ip);
        cyw43_arch_lwip_end();
    }
    
    pbuf_free(p);
}

void ping_test(const char* host) {
    if (!wifi_connected) {
        printf(ANSI_RED "WiFi not connected. Connect to WiFi first.\n" ANSI_RESET);
        return;
    }
    
    printf("\n" ANSI_BOLD "PING %s" ANSI_RESET "\n", host);
    
    // Resolve hostname or parse IP
    ip_addr_t target_ip;
    if (!ipaddr_aton(host, &target_ip)) {
        printf("Resolving hostname %s...\n", host);
        
        // Simple DNS resolution with timeout
        struct {
            bool resolved;
            ip_addr_t addr;
        } dns_data = {false, {0}};
        
        auto dns_callback = [](const char *name, const ip_addr_t *ipaddr, void *arg) {
            auto *data = (decltype(dns_data)*)arg;
            if (ipaddr) {
                data->addr = *ipaddr;
                data->resolved = true;
            }
        };
        
        cyw43_arch_lwip_begin();
        err_t err = dns_gethostbyname(host, &target_ip, dns_callback, &dns_data);
        cyw43_arch_lwip_end();
        
        if (err == ERR_INPROGRESS) {
            // Wait for DNS resolution
            uint32_t start = to_ms_since_boot(get_absolute_time());
            while (!dns_data.resolved && (to_ms_since_boot(get_absolute_time()) - start < 5000)) {
                sleep_ms(100);
            }
            
            if (dns_data.resolved) {
                target_ip = dns_data.addr;
            } else {
                printf(ANSI_RED "DNS resolution timeout\n" ANSI_RESET);
                return;
            }
        } else if (err != ERR_OK) {
            printf(ANSI_RED "DNS resolution failed\n" ANSI_RESET);
            return;
        }
    }
    
    printf("Target: %s\n", ipaddr_ntoa(&target_ip));
    printf("Sending 4 ICMP echo requests...\n\n");
    
    // Setup ping
    ping_data.target_ip = target_ip;
    ping_data.seq_num = 0;
    
    cyw43_arch_lwip_begin();
    ping_data.pcb = raw_new(IP_PROTO_ICMP);
    cyw43_arch_lwip_end();
    
    if (!ping_data.pcb) {
        printf(ANSI_RED "Failed to create ICMP socket\n" ANSI_RESET);
        return;
    }
    
    cyw43_arch_lwip_begin();
    raw_recv(ping_data.pcb, ping_recv, &ping_data);
    raw_bind(ping_data.pcb, IP_ADDR_ANY);
    cyw43_arch_lwip_end();
    
    int received_count = 0;
    uint32_t total_rtt = 0;
    uint32_t min_rtt = UINT32_MAX;
    uint32_t max_rtt = 0;
    
    // Send 4 pings
    for (int i = 0; i < 4; i++) {
        ping_data.seq_num = i + 1;
        ping_send(&ping_data);
        
        // Wait for response (1 second timeout)
        uint32_t start = to_ms_since_boot(get_absolute_time());
        while (!ping_data.received && (to_ms_since_boot(get_absolute_time()) - start < 1000)) {
            sleep_ms(10);
        }
        
        if (ping_data.received) {
            printf("%s: icmp_seq=%d time=%lu ms\n", 
                   ipaddr_ntoa(&target_ip), ping_data.seq_num, (unsigned long)ping_data.rtt_ms);
            received_count++;
            total_rtt += ping_data.rtt_ms;
            if (ping_data.rtt_ms < min_rtt) min_rtt = ping_data.rtt_ms;
            if (ping_data.rtt_ms > max_rtt) max_rtt = ping_data.rtt_ms;
        } else {
            printf("%s: icmp_seq=%d " ANSI_RED "Request timeout" ANSI_RESET "\n", 
                   ipaddr_ntoa(&target_ip), ping_data.seq_num);
        }
        
        sleep_ms(1000);
    }
    
    // Cleanup
    cyw43_arch_lwip_begin();
    raw_remove(ping_data.pcb);
    cyw43_arch_lwip_end();
    
    // Print statistics
    printf("\n--- %s ping statistics ---\n", host);
    printf("4 packets transmitted, %d received, %d%% packet loss\n", 
           received_count, ((4 - received_count) * 100) / 4);
    
    if (received_count > 0) {
        printf("rtt min/avg/max = %lu/%lu/%lu ms\n", 
               (unsigned long)min_rtt, 
               (unsigned long)(total_rtt / received_count), 
               (unsigned long)max_rtt);
    }
    printf("\n");
}

void connect_wifi() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║          WiFi Configuration            ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    
    char *ssid = read_line("\nEnter WiFi SSID: ", true);
    if (!ssid || strlen(ssid) == 0) {
        printf(ANSI_RED "Error: SSID cannot be empty\n" ANSI_RESET);
        return;
    }
    
    char *password = read_line("Enter WiFi Password: ", false);
    if (!password) {
        printf(ANSI_RED "Error: Password input failed\n" ANSI_RESET);
        return;
    }
    
    printf("\n");
    
    strncpy(wifi_ssid, ssid, sizeof(wifi_ssid) - 1);
    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
    strncpy(wifi_password, password, sizeof(wifi_password) - 1);
    wifi_password[sizeof(wifi_password) - 1] = '\0';
    
    printf("Connecting to WiFi...\n");
    printf("SSID: %s\n", wifi_ssid);
    
    cyw43_arch_enable_sta_mode();
    
    // Try multiple authentication methods
    int result = -1;
    
    // Try WPA2 first (most common)
    printf("Trying WPA2...\n");
    result = cyw43_arch_wifi_connect_timeout_ms(wifi_ssid, wifi_password, 
                                                  CYW43_AUTH_WPA2_AES_PSK, 15000);
    
    if (result != 0) {
        printf("WPA2 failed, trying WPA2 Mixed...\n");
        result = cyw43_arch_wifi_connect_timeout_ms(wifi_ssid, wifi_password, 
                                                      CYW43_AUTH_WPA2_MIXED_PSK, 15000);
    }
    
    if (result != 0) {
        printf("Trying WPA...\n");
        result = cyw43_arch_wifi_connect_timeout_ms(wifi_ssid, wifi_password, 
                                                      CYW43_AUTH_WPA_TKIP_PSK, 15000);
    }
    
    if (result == 0) {
        wifi_connected = true;
        printf(ANSI_GREEN "\n✓ Connected successfully!\n" ANSI_RESET);
        
        // Show IP address
        sleep_ms(1000);
        const ip4_addr_t *addr_ptr = netif_ip4_addr(netif_list);
        if (addr_ptr) {
            printf("IP Address: %s\n", ip4addr_ntoa(addr_ptr));
        }
        
        // Save config
        lfs_file_t file;
        if (lfs_file_open(&lfs, &file, "wifi.cfg", LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) >= 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s\n%s", wifi_ssid, wifi_password);
            lfs_file_write(&lfs, &file, buf, strlen(buf));
            lfs_file_close(&lfs, &file);
        }
        
        log_message("WiFi connected");
        
        // Sync time
        printf("Syncing time...\n");
        sync_ntp_time();
    } else {
        wifi_connected = false;
        printf(ANSI_RED "\n✗ Connection failed!\n" ANSI_RESET);
        printf("\nTroubleshooting:\n");
        printf("  • Check SSID is correct (case-sensitive)\n");
        printf("  • Check password is correct\n");
        printf("  • Make sure network is 2.4GHz (not 5GHz)\n");
        printf("  • Try moving closer to the router\n");
        printf("  • Check if MAC filtering is enabled\n");
        log_message("WiFi connection failed");
    }
}

void show_time() {
    time_t now = get_current_time();
    if (now == 0) {
        printf(ANSI_YELLOW "Time not synchronized yet\n" ANSI_RESET);
        printf("Use 'wifi' to connect and sync time\n");
    } else {
        struct tm *t = localtime(&now);
        printf("\n" ANSI_BOLD "Current Time:\n" ANSI_RESET);
        printf("  %04d-%02d-%02d %02d:%02d:%02d %s\n\n",
               t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
               t->tm_hour, t->tm_min, t->tm_sec, timezone_str);
    }
}

void view_log() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║            System Logs                 ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    printf("\n");
    
    if (log_count == 0) {
        printf("No log entries yet\n");
    } else {
        int start = (log_index - log_count + MAX_LOG_ENTRIES) % MAX_LOG_ENTRIES;
        for (int i = 0; i < log_count; i++) {
            int idx = (start + i) % MAX_LOG_ENTRIES;
            printf("%s\n", log_entries[idx]);
        }
    }
    
    printf("\n");
}

void show_ram() {
    printf("\n" ANSI_BOLD "Memory Information:\n" ANSI_RESET);
    printf("  Total RAM: 520 KB\n");
    printf("  Stack: 4 KB\n");
    printf("  Heap: 16 KB\n");
    printf("\n");
}

void settings_menu() {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_BOLD ANSI_CYAN "╔════════════════════════════════════════╗\n");
    printf("║             Settings Menu              ║\n");
    printf("╚════════════════════════════════════════╝\n" ANSI_RESET);
    
    printf("\n1. Sync time with NTP\n");
    printf("2. Set timezone\n");
    printf("3. Clear WiFi credentials\n");
    printf("4. Format filesystem\n");
    printf("5. Exit\n");
    
    char *choice = read_line("\nChoice: ", true);
    
    if (strcmp(choice, "1") == 0) {
        sync_ntp_time();
    } else if (strcmp(choice, "2") == 0) {
        char *tz = read_line("Enter timezone offset (e.g., 0 for GMT, 1 for BST): ", true);
        timezone_offset = atoi(tz);
        printf(ANSI_GREEN "Timezone set to GMT%+d\n" ANSI_RESET, timezone_offset);
    } else if (strcmp(choice, "3") == 0) {
        wifi_ssid[0] = '\0';
        wifi_password[0] = '\0';
        lfs_remove(&lfs, "wifi.cfg");
        printf(ANSI_GREEN "WiFi credentials cleared\n" ANSI_RESET);
    } else if (strcmp(choice, "4") == 0) {
        printf(ANSI_RED "WARNING: This will erase all files!\n" ANSI_RESET);
        char *confirm = read_line("Type 'yes' to confirm: ", true);
        if (strcmp(confirm, "yes") == 0) {
            lfs_unmount(&lfs);
            lfs_format(&lfs, &lfs_cfg);
            lfs_mount(&lfs, &lfs_cfg);
            printf(ANSI_GREEN "Filesystem formatted\n" ANSI_RESET);
        }
    }
}

char* read_line(const char* prompt, bool echo) {
    static char input_buffer1[256];
    static char input_buffer2[256];
    static int buffer_toggle = 0;
    
    // Alternate between two buffers to avoid overwriting previous input
    char *current_buffer = (buffer_toggle == 0) ? input_buffer1 : input_buffer2;
    buffer_toggle = 1 - buffer_toggle;
    
    size_t idx = 0;
    
    printf("%s", prompt);
    fflush(stdout);
    
    while (idx < sizeof(input_buffer1) - 1) {
        int c = getchar();
        
        if (c == '\r' || c == '\n') {
            current_buffer[idx] = '\0';
            printf("\r\n");
            return current_buffer;
        } else if (c == 127 || c == 8) {
            if (idx > 0) {
                idx--;
                if (echo) {
                    printf("\b \b");
                }
            }
        } else if (c >= 32 && c < 127) {
            current_buffer[idx++] = c;
            if (echo) {
                putchar(c);
            } else {
                putchar('*');
            }
        }
        fflush(stdout);
    }
    
    current_buffer[idx] = '\0';
    printf("\r\n");
    return current_buffer;
}

// Parse and execute command
void execute_command(char* cmd) {
    char* args[MAX_ARGS];
    int argc = 0;
    
    char* token = strtok(cmd, " ");
    while (token != NULL && argc < MAX_ARGS) {
        args[argc++] = token;
        token = strtok(NULL, " ");
    }
    
    if (argc == 0) return;
    
    if (strcmp(args[0], "help") == 0) {
        show_help();
    } else if (strcmp(args[0], "neofetch") == 0) {
        neofetch();
        read_line("\nPress Enter to continue...", true);
    } else if (strcmp(args[0], "timer") == 0) {
        timer_app();
    } else if (strcmp(args[0], "todo") == 0) {
        todo_app();
    } else if (strcmp(args[0], "nmap") == 0) {
        nmap_app();
    } else if (strcmp(args[0], "ascii") == 0) {
        ascii_converter();
    } else if (strcmp(args[0], "tetris") == 0) {
        tetris_game();
    } else if (strcmp(args[0], "snake") == 0) {
        snake_game();
    } else if (strcmp(args[0], "sysinfo") == 0) {
        show_system_info();
    } else if (strcmp(args[0], "clear") == 0) {
        printf(ANSI_CLEAR_SCREEN);
    } else if (strcmp(args[0], "reboot") == 0) {
        printf("Rebooting...\n");
        sleep_ms(1000);
        watchdog_enable(1, 1);
        while (1);
    } else if (strcmp(args[0], "ps") == 0) {
        list_processes();
    } else if (strcmp(args[0], "stop") == 0) {
        if (argc < 2) {
            printf("Usage: stop <process_name>\n");
        } else {
            stop_process(args[1]);
        }
    } else if (strcmp(args[0], "ls") == 0) {
        list_files();
    } else if (strcmp(args[0], "cat") == 0) {
        if (argc < 2) {
            printf("Usage: cat <filename>\n");
        } else {
            view_file(args[1]);
        }
    } else if (strcmp(args[0], "nano") == 0) {
        if (argc < 2) {
            printf("Usage: nano <filename>\n");
        } else {
            nano_editor(args[1]);
        }
    } else if (strcmp(args[0], "make") == 0) {
        if (argc < 2) {
            printf("Usage: make <filename>\n");
        } else {
            nano_editor(args[1]);
        }
    } else if (strcmp(args[0], "delete") == 0) {
        if (argc < 2) {
            printf("Usage: delete <filename>\n");
        } else {
            delete_file(args[1]);
        }
    } else if (strcmp(args[0], "showspace") == 0) {
        show_storage_info();
    } else if (strcmp(args[0], "ipa") == 0) {
        show_ip();
    } else if (strcmp(args[0], "ping") == 0) {
        if (argc < 2) {
            printf("Usage: ping <host>\n");
        } else {
            ping_test(args[1]);
        }
    } else if (strcmp(args[0], "wifi") == 0) {
        connect_wifi();
    } else if (strcmp(args[0], "time") == 0) {
        show_time();
    } else if (strcmp(args[0], "viewlog") == 0) {
        view_log();
    } else if (strcmp(args[0], "showram") == 0) {
        show_ram();
    } else if (strcmp(args[0], "setting") == 0) {
        settings_menu();
    } else if (strcmp(args[0], "localhost") == 0) {
        start_http_server();
    } else if (strcmp(args[0], "stopweb") == 0) {
        stop_http_server();
    } else if (strcmp(args[0], "createweb") == 0) {
        create_default_website();
    } else {
        printf(ANSI_RED "Unknown command: %s" ANSI_RESET "\n", args[0]);
        printf("Type 'help' for available commands\n");
    }
}

// Print shell prompt
void print_prompt() {
    time_t now = get_current_time();
    if (now == 0) {
        // No time set, show uptime
        uint32_t uptime_sec = to_ms_since_boot(get_absolute_time()) / 1000;
        printf(ANSI_GREEN "+%05lus" ANSI_RESET " " 
               ANSI_BOLD ANSI_BLUE "pico@os" ANSI_RESET 
               ":" ANSI_CYAN "~" ANSI_RESET "$ ", (unsigned long)uptime_sec);
    } else {
        struct tm *t = localtime(&now);
        printf(ANSI_GREEN "%02d:%02d:%02d" ANSI_RESET " " 
               ANSI_BOLD ANSI_BLUE "pico@os" ANSI_RESET 
               ":" ANSI_CYAN "~" ANSI_RESET "$ ", 
               t->tm_hour, t->tm_min, t->tm_sec);
    }
    fflush(stdout);
}

// Main shell loop
void shell_loop() {
    print_prompt();
    
    while (true) {
        int c = getchar_timeout_us(0);
        
        if (c == PICO_ERROR_TIMEOUT) {
            sleep_ms(10);
            continue;
        }
        
        if (c == '\r' || c == '\n') {
            command_buffer[cmd_index] = '\0';
            printf("\r\n");
            
            if (cmd_index > 0) {
                execute_command(command_buffer);
            }
            
            cmd_index = 0;
            command_buffer[0] = '\0';
            print_prompt();
        } else if (c == 127 || c == 8) { // Backspace
            if (cmd_index > 0) {
                cmd_index--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c >= 32 && c < 127) {
            if (cmd_index < MAX_COMMAND_LEN - 1) {
                command_buffer[cmd_index++] = c;
                putchar(c);
                fflush(stdout);
            }
        }
    }
}

// Background NTP sync task
void ntp_sync_task() {
    while (true) {
        if (wifi_connected && ntp_synced) {
            sleep_ms(3600000); // Sync every hour
            sync_ntp_time();
        } else {
            sleep_ms(5000);
        }
    }
}

// Boot sequence
void boot_sequence() {
    printf("\r\n\r\n");
    printf("╔═══════════════════════════════════════════════╗\r\n");
    printf("║     Raspberry Pi Pico 2 W Operating System   ║\r\n");
    printf("║                  Version 2.0                  ║\r\n");
    printf("║         🌐 Now with Web Server! 🌐           ║\r\n");
    printf("╚═══════════════════════════════════════════════╝\r\n");
    printf("\r\n");
    
    printf("Booting...\r\n\r\n");
    
    log_message("System booting");
    
    printf("[OK] Initializing hardware\r\n");
    
    printf("[..] Mounting filesystem\r\n");
    init_filesystem();
    printf("[OK] Filesystem ready\r\n");
    
    printf("[..] Starting WiFi driver\r\n");
    int wifi_init = cyw43_arch_init();
    if (wifi_init != 0) {
        printf("[WARN] WiFi driver init failed (code %d)\r\n", wifi_init);
        printf("[WARN] WiFi features will be unavailable\r\n");
        log_message("WARNING: WiFi init failed");
    } else {
        cyw43_arch_enable_sta_mode();
        printf("[OK] WiFi driver ready\r\n");
    }
    
    printf("[OK] Initializing system clock\r\n");
    // Initialize time tracking
    time_sync_base = get_absolute_time();
    system_time_offset = 0; // Will be set by NTP
    
    // Load WiFi config
    lfs_file_t file;
    if (lfs_file_open(&lfs, &file, "wifi.cfg", LFS_O_RDONLY) >= 0) {
        char buf[128];
        int len = lfs_file_read(&lfs, &file, buf, sizeof(buf) - 1);
        if (len > 0) {
            buf[len] = '\0';
            char* newline = strchr(buf, '\n');
            if (newline) {
                *newline = '\0';
                size_t ssid_len = strlen(buf);
                if (ssid_len < sizeof(wifi_ssid)) {
                    memcpy(wifi_ssid, buf, ssid_len);
                    wifi_ssid[ssid_len] = '\0';
                }
                size_t pass_len = strlen(newline + 1);
                if (pass_len < sizeof(wifi_password)) {
                    memcpy(wifi_password, newline + 1, pass_len);
                    wifi_password[pass_len] = '\0';
                }
                printf("[OK] WiFi credentials loaded\r\n");
            }
        }
        lfs_file_close(&lfs, &file);
    }
    
    printf("\r\nBoot complete!\r\n");
    printf("Type 'help' for available commands\r\n");
    printf("Type 'neofetch' for a cool system overview\r\n");
    printf(ANSI_GREEN "NEW in v2.0: Type 'localhost' to start web server!\r\n" ANSI_RESET);
    printf("Apps: 'nmap', 'ascii', 'tetris', 'snake', 'timer', 'todo'\r\n\r\n");
    
    log_message("Boot complete");
    boot_time = get_absolute_time();
}

// Main function
int main() {
    // Initialize all stdio types
    stdio_init_all();
    
    // Critical: Wait for USB to enumerate
    // The Pico needs time to set up USB CDC
    busy_wait_ms(2000);
    
    // Send test pattern to verify USB is working
    for (int i = 0; i < 10; i++) {
        printf("\r\n");
        busy_wait_ms(50);
    }
    
    printf("╔════════════════════════════════╗\r\n");
    printf("║   USB SERIAL ACTIVE - TEST OK  ║\r\n");
    printf("╚════════════════════════════════╝\r\n");
    printf("\r\n");
    busy_wait_ms(500);
    
    printf("Pico OS initializing...\r\n");
    printf("If you see this, USB serial is working!\r\n\r\n");
    busy_wait_ms(500);
    
    // Initialize random seed
    srand(to_ms_since_boot(get_absolute_time()));
    
    // Boot sequence with error checking
    boot_sequence();
    
    printf("Starting background tasks...\r\n");
    
    // Start background tasks
    int ntp_pid = add_process("ntp_sync", ntp_sync_task);
    if (ntp_pid < 0) {
        printf("WARNING: Failed to start NTP sync task\r\n");
    }
    
    printf("Entering shell...\r\n\r\n");
    busy_wait_ms(300);
    
    // Enter shell loop
    shell_loop();
    
    // Should never reach here
    panic_handler("Shell loop exited unexpectedly");
    
    return 0;
}
