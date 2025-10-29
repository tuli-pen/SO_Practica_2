#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <termios.h>
#include <ctype.h>

#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define RECV_BUF_SZ 16384

// ---------------------- UTILIDADES ----------------------
ssize_t readn(int fd, void *buf, size_t n) {
    size_t left = n; char *p = buf;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) { if (errno==EINTR) continue; return -1; }
        if (r==0) break;
        left -= r; p += r;
    }
    return (n-left);
}

ssize_t writen(int fd, const void *buf, size_t n) {
    size_t left = n; const char *p = buf;
    while (left>0) {
        ssize_t w = write(fd, p, left);
        if (w<=0) { if (w<0 && errno==EINTR) continue; return -1; }
        left -= w; p += w;
    }
    return n;
}

void trim_newline(char *s) {
    if (!s) return;
    size_t l = strlen(s);
    while(l>0 && (s[l-1]=='\n'||s[l-1]=='\r')) { s[l-1]='\0'; l--; }
}

// ---------------------- RAW MODE ----------------------
static struct termios orig_termios;

void disable_raw_mode(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

int enable_raw_mode(void) {
    if(tcgetattr(STDIN_FILENO,&orig_termios)==-1) return -1;
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;
    return tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw);
}

int read_key(void) {
    unsigned char c;
    if(read(STDIN_FILENO,&c,1)!=1) return -1;
    if(c==0x1b) {
        unsigned char seq[2];
        if(read(STDIN_FILENO,&seq[0],1)!=1) return 27;
        if(read(STDIN_FILENO,&seq[1],1)!=1) return 27;
        if(seq[0]=='['){
            if(seq[1]=='C') return 1001; // derecha
            if(seq[1]=='D') return 1002; // izquierda
        }
        return 27;
    }
    return (int)c;
}

// ---------------------- CONEXION AL SERVIDOR ----------------------
int connect_server(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if(sock<0){ perror("socket"); return -1; }
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    if(inet_pton(AF_INET,SERVER_IP,&addr.sin_addr)<=0){ perror("inet_pton"); close(sock); return -1; }
    if(connect(sock,(struct sockaddr*)&addr,sizeof(addr))<0){ perror("connect"); close(sock); return -1; }
    return sock;
}

int send_command_and_receive(int cmd, const char *payload, char *out, size_t outsz) {
    if(!payload) payload="";
    int sock = connect_server();
    if(sock<0) return -1;

    uint32_t cmd_net = htonl(cmd);
    uint32_t len_net = htonl((uint32_t)strlen(payload));
    if(writen(sock,&cmd_net,sizeof(cmd_net))!=sizeof(cmd_net) ||
       writen(sock,&len_net,sizeof(len_net))!=sizeof(len_net) ||
       (strlen(payload)>0 && writen(sock,payload,strlen(payload))!=(ssize_t)strlen(payload))) {
        perror("send_command"); close(sock); return -1;
    }

    uint32_t resp_len_net;
    if(readn(sock,&resp_len_net,sizeof(resp_len_net))!=sizeof(resp_len_net)){ close(sock); return -1; }
    uint32_t resp_len = ntohl(resp_len_net);
    if(resp_len >= outsz) resp_len = (uint32_t)outsz-1;
    if(readn(sock,out,resp_len)!=(ssize_t)resp_len){ close(sock); return -1; }
    out[resp_len]='\0';
    close(sock);
    return 0;
}

// ---------------------- BÚSQUEDA INTERACTIVA ----------------------
void search_interactive(const char *q) {
    char reply[RECV_BUF_SZ];

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (send_command_and_receive(1, q, reply, sizeof(reply)) != 0) {
        printf("Error consultando al servidor.\n");
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/1e9;
    printf("[Búsqueda realizada en %.3f segundos]\n", elapsed);

    // Separar resultados en líneas
    char *lines[1024];
    int nlines = 0;
    char *p = reply;
    while (*p && nlines < 1024) {
        lines[nlines++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }

    if (nlines == 0) {
        printf("No se encontraron resultados.\n");
        return;
    }

    int index = 0;  // mostrar uno a uno
    for (;;) {
        printf("\n===== Resultado %d/%d =====\n%s\n", index+1, nlines, lines[index]);
        printf("[← anterior | → siguiente | q salir]\n");

        if (enable_raw_mode() == -1) { perror("raw"); return; }
        int k = read_key();
        disable_raw_mode();

        if (k == 'q' || k == 'Q' || k == 27) break;
        else if (k == 1001 && index < nlines - 1) index++;   // flecha derecha → siguiente
        else if (k == 1002 && index > 0) index--;            // flecha izquierda ← anterior
    }
}

// ---------------------- CSV ----------------------
void make_csv_line(char *dst, size_t dst_sz,
                   const char *id, const char *submitter, const char *authors,
                   const char *title, const char *abstract, const char *categories,
                   const char *comments, const char *journal_ref, const char *doi,
                   const char *report_no, const char *license, const char *update_date,
                   const char *versions_count, const char *versions_last_created){
    snprintf(dst,dst_sz,"\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"",
             id,submitter,authors,title,abstract,categories,comments,journal_ref,doi,report_no,license,update_date,versions_count,versions_last_created);
}

// ---------------------- MAIN ----------------------
int main(void){
    while(1){
        printf("\n===== CLIENTE UI =====\n1) Buscar\n2) Insertar\n3) Salir\nElija opción: ");
        int opt=0;
        if(scanf("%d",&opt)!=1){ while(getchar()!='\n'); continue; }
        while(getchar()!='\n');

        if(opt==1){
            char q[512];
            printf("Ingrese palabra clave: "); if(!fgets(q,sizeof(q),stdin)) continue;
            trim_newline(q); if(strlen(q)==0){ printf("Cadena vacía.\n"); continue; }
            search_interactive(q);
        }
        else if(opt==2){
            char id[64]="", submitter[128]="", authors[256]="", title[256]="", abstract[512]="";
            char categories[128]="", comments[128]="", journal_ref[128]="", doi[128]="";
            char report_no[64]="", license[64]="", update_date[64]="", versions_count[16]="", versions_last_created[64]="";

            printf("Ingrese datos del nuevo registro:\n");
            printf("ID: "); fgets(id,sizeof(id),stdin); trim_newline(id);
            printf("Submitter: "); fgets(submitter,sizeof(submitter),stdin); trim_newline(submitter);
            printf("Authors: "); fgets(authors,sizeof(authors),stdin); trim_newline(authors);
            printf("Title: "); fgets(title,sizeof(title),stdin); trim_newline(title);
            printf("Abstract: "); fgets(abstract,sizeof(abstract),stdin); trim_newline(abstract);
            printf("Categories: "); fgets(categories,sizeof(categories),stdin); trim_newline(categories);
            printf("Comments: "); fgets(comments,sizeof(comments),stdin); trim_newline(comments);
            printf("Journal-ref: "); fgets(journal_ref,sizeof(journal_ref),stdin); trim_newline(journal_ref);
            printf("DOI: "); fgets(doi,sizeof(doi),stdin); trim_newline(doi);
            printf("Report-no: "); fgets(report_no,sizeof(report_no),stdin); trim_newline(report_no);
            printf("License: "); fgets(license,sizeof(license),stdin); trim_newline(license);
            printf("Update date: "); fgets(update_date,sizeof(update_date),stdin); trim_newline(update_date);
            printf("Versions count: "); fgets(versions_count,sizeof(versions_count),stdin); trim_newline(versions_count);
            printf("Versions last created: "); fgets(versions_last_created,sizeof(versions_last_created),stdin); trim_newline(versions_last_created);

            char csv_line[4096];
            make_csv_line(csv_line,sizeof(csv_line),id,submitter,authors,title,abstract,categories,comments,
                          journal_ref,doi,report_no,license,update_date,versions_count,versions_last_created);

            char reply[RECV_BUF_SZ];
            if(send_command_and_receive(2,csv_line,reply,sizeof(reply))==0) printf("Insertado con éxito.\n");
            else printf("Error comunicándose con el servidor.\n");
        }
        else if(opt==3){
            printf("Saliendo...\n"); break;
        }
        else printf("Opción inválida.\n");
    }
    return 0;
}
