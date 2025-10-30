#define _POSIX_C_SOURCE 200809L   // Pido funciones POSIX modernas (p.ej., clock_gettime, tcgetattr)
#include <time.h>                 // Tiempos (clock_gettime)
#include <stdio.h>                // printf, scanf, fgets, perror...
#include <stdlib.h>               // general (exit, malloc si hiciera falta)
#include <string.h>               // strlen, memset, strchr...
#include <unistd.h>               // read, write, close, STDIN_FILENO
#include <errno.h>                // errno y códigos de error
#include <arpa/inet.h>            // htonl, ntohl, inet_pton (conversiones red/host)
#include <sys/socket.h>           // socket, connect, send/recv (nivel sockets)
#include <netinet/in.h>           // struct sockaddr_in y familia AF_INET
#include <termios.h>              // manejo de modo raw en la terminal
#include <ctype.h>                // utilidades de caracteres (no se usa mucho aquí)

#define SERVER_IP "127.0.0.1"     // IP del servidor (mismo equipo: loopback)
#define SERVER_PORT 12345         // Puerto al que conectamos
#define RECV_BUF_SZ 16384         // Tamaño buffer para respuestas del servidor

// ---------------------- UTILIDADES ----------------------
// readn: leer exactamente 'n' bytes o hasta EOF. Maneja interrupciones (EINTR).
ssize_t readn(int fd, void *buf, size_t n) {
    size_t left = n; char *p = buf;              // left = lo que falta; p avanza en el buffer
    while (left > 0) {
        ssize_t r = read(fd, p, left);           // intento leer lo que falta
        if (r < 0) {                             // error al leer
            if (errno==EINTR) continue;          // si fue interrumpido, reintento
            return -1;                           // otro error: salgo
        }
        if (r==0) break;                         // EOF: el peer cerró
        left -= r; p += r;                       // avanzo puntero y resto bytes leídos
    }
    return (n-left);                              // devuelvo cuántos bytes logré leer
}

// writen: escribir exactamente 'n' bytes. Similar a readn pero para write.
ssize_t writen(int fd, const void *buf, size_t n) {
    size_t left = n; const char *p = buf;
    while (left>0) {
        ssize_t w = write(fd, p, left);
        if (w<=0) {                              // error o 0 (raro en write)
            if (w<0 && errno==EINTR) continue;   // interrumpido: reintento
            return -1;                           // otro error: salgo
        }
        left -= w; p += w;                       // avanzo lo escrito
    }
    return n;                                    // se escribieron todos los bytes
}

// trim_newline: quita '\n' o '\r' al final de una cadena (limpia input de fgets)
void trim_newline(char *s) {
    if (!s) return;                              // seguridad por si s==NULL
    size_t l = strlen(s);
    while(l>0 && (s[l-1]=='\n'||s[l-1]=='\r')) { // mientras el último sea salto o CR
        s[l-1]='\0'; l--;                        // los corto
    }
}

// ---------------------- RAW MODE ----------------------
// Guardamos la configuración original del terminal para restaurarla luego
static struct termios orig_termios;

// Vuelve a poner la terminal en modo normal cuando salgamos
void disable_raw_mode(void) { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

int enable_raw_mode(void) {
    if(tcgetattr(STDIN_FILENO,&orig_termios)==-1) return -1; // leo config actual
    atexit(disable_raw_mode);                                 // garantizo restauración al salir
    struct termios raw = orig_termios;                        // copio la config
    raw.c_lflag &= ~(ECHO | ICANON);                          // apago eco y modo canónico (leo por carácter)
    raw.c_cc[VMIN]=1; raw.c_cc[VTIME]=0;                      // bloqueo hasta 1 byte; sin timeout
    return tcsetattr(STDIN_FILENO,TCSAFLUSH,&raw);            // aplico modo raw
}

// Lee una tecla, decodifica flechas ← → (secuencias ESC [ C / ESC [ D)
int read_key(void) {
    unsigned char c;
    if(read(STDIN_FILENO,&c,1)!=1) return -1;   // leo 1 byte del teclado
    if(c==0x1b) {                               // 0x1b = ESC, posible inicio de secuencia
        unsigned char seq[2];
        if(read(STDIN_FILENO,&seq[0],1)!=1) return 27;  // si falla, devuelvo ESC
        if(read(STDIN_FILENO,&seq[1],1)!=1) return 27;
        if(seq[0]=='['){
            if(seq[1]=='C') return 1001; // flecha derecha → (código propio)
            if(seq[1]=='D') return 1002; // flecha izquierda ← (código propio)
        }
        return 27;                        // otra secuencia: lo trato como ESC
    }
    return (int)c;                        // tecla normal (p. ej. 'q')
}

// ---------------------- CONEXION AL SERVIDOR ----------------------
// Abre un socket TCP y conecta con SERVER_IP:SERVER_PORT
int connect_server(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);         // socket TCP IPv4
    if(sock<0){ perror("socket"); return -1; }
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));                       // limpio struct
    addr.sin_family = AF_INET;                          // IPv4
    addr.sin_port = htons(SERVER_PORT);                 // puerto en endianness de red
    if(inet_pton(AF_INET,SERVER_IP,&addr.sin_addr)<=0){ // texto IP -> binario
        perror("inet_pton"); close(sock); return -1;
    }
    if(connect(sock,(struct sockaddr*)&addr,sizeof(addr))<0){ // intento conectar
        perror("connect"); close(sock); return -1;
    }
    return sock;                                        // devuelvo fd del socket
}

// Protocolo app muy simple: mando (cmd:uint32_be)(len:uint32_be)(payload bytes)
// El server responde: (resp_len:uint32_be)(resp_bytes)
int send_command_and_receive(int cmd, const char *payload, char *out, size_t outsz) {
    if(!payload) payload="";                            // payload nulo -> vacío
    int sock = connect_server();                        // abro conexión
    if(sock<0) return -1;

    uint32_t cmd_net = htonl(cmd);                      // convierto a big-endian de red
    uint32_t len_net = htonl((uint32_t)strlen(payload));
    // envío cabecera y payload (si hay)
    if(writen(sock,&cmd_net,sizeof(cmd_net))!=sizeof(cmd_net) ||
       writen(sock,&len_net,sizeof(len_net))!=sizeof(len_net) ||
       (strlen(payload)>0 && writen(sock,payload,strlen(payload))!=(ssize_t)strlen(payload))) {
        perror("send_command"); close(sock); return -1; // error en envío
    }

    // leo primero el tamaño de la respuesta
    uint32_t resp_len_net;
    if(readn(sock,&resp_len_net,sizeof(resp_len_net))!=sizeof(resp_len_net)){ close(sock); return -1; }
    uint32_t resp_len = ntohl(resp_len_net);            // paso a endianness host

    // evito overflow en 'out' y garantizo espacio para '\0'
    if(resp_len >= outsz) resp_len = (uint32_t)outsz-1;

    // leo la respuesta en 'out'
    if(readn(sock,out,resp_len)!=(ssize_t)resp_len){ close(sock); return -1; }
    out[resp_len]='\0';                                 // aseguro cadena terminada
    close(sock);                                        // cierro socket (protocolo por-request)
    return 0;                                           // OK
}

// ---------------------- BÚSQUEDA INTERACTIVA ----------------------
void search_interactive(const char *q) {
    char reply[RECV_BUF_SZ];                            // buffer respuesta

    struct timespec start, end;                         // medir duración
    clock_gettime(CLOCK_MONOTONIC, &start);             // tiempo inicio

    // cmd=1: “Buscar”. payload = query 'q'
    if (send_command_and_receive(1, q, reply, sizeof(reply)) != 0) {
        printf("Error consultando al servidor.\n");
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);               // tiempo fin
    double elapsed = (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec)/1e9;
    printf("[Búsqueda realizada en %.3f segundos]\n", elapsed);

    // Parto la respuesta por líneas para paginarlas con ← →
    char *lines[1024];
    int nlines = 0;
    char *p = reply;
    while (*p && nlines < 1024) {
        lines[nlines++] = p;                            // guardo inicio de línea
        char *nl = strchr(p, '\n');                     // busco salto
        if (!nl) break;                                 // si no hay más, salgo
        *nl = '\0';                                     // corto la línea
        p = nl + 1;                                     // avanzo al siguiente inicio
    }

    if (nlines == 0) {
        printf("No se encontraron resultados.\n");
        return;
    }

    int index = 0;                                      // índice del resultado actual
    for (;;) {
        printf("\n===== Resultado %d/%d =====\n%s\n", index+1, nlines, lines[index]);
        printf("[← anterior | → siguiente | q salir]\n");

        if (enable_raw_mode() == -1) { perror("raw"); return; }  // entro en modo tecla-a-tecla
        int k = read_key();                                      // leo una tecla
        disable_raw_mode();                                      // restauro terminal

        if (k == 'q' || k == 'Q' || k == 27) break;              // q o ESC -> salir
        else if (k == 1001 && index < nlines - 1) index++;       // →: siguiente
        else if (k == 1002 && index > 0) index--;                // ←: anterior
    }
}

// ---------------------- CSV ----------------------
// Arma una línea CSV con 14 campos, ya con comillas y separadas por comas
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
    while(1){                                                      // loop menú
        printf("\n===== CLIENTE UI =====\n1) Buscar\n2) Insertar\n3) Salir\nElija opción: ");
        int opt=0;
        if(scanf("%d",&opt)!=1){ while(getchar()!='\n'); continue; } // leo opción; limpio basura si falla
        while(getchar()!='\n');                                      // consumo el '\n' que queda

        if(opt==1){
            char q[512];
            printf("Ingrese palabra clave: "); if(!fgets(q,sizeof(q),stdin)) continue; // leo query
            trim_newline(q); if(strlen(q)==0){ printf("Cadena vacía.\n"); continue; }  // valido
            search_interactive(q);                                                     // ejecuto búsqueda
        }
        else if(opt==2){
            // buffers para cada campo del registro nuevo
            char id[64]="", submitter[128]="", authors[256]="", title[256]="", abstract[512]="";
            char categories[128]="", comments[128]="", journal_ref[128]="", doi[128]="";
            char report_no[64]="", license[64]="", update_date[64]="", versions_count[16]="", versions_last_created[64]="";

            // pido todos los datos por consola
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

            // armo la línea CSV con todos los campos
            char csv_line[4096];
            make_csv_line(csv_line,sizeof(csv_line),id,submitter,authors,title,abstract,categories,comments,
                          journal_ref,doi,report_no,license,update_date,versions_count,versions_last_created);

            // mando cmd=2 (Insertar) + payload=csv_line; leo respuesta
            char reply[RECV_BUF_SZ];
            if(send_command_and_receive(2,csv_line,reply,sizeof(reply))==0) printf("Insertado con éxito.\n");
            else printf("Error comunicándose con el servidor.\n");
        }
        else if(opt==3){
            printf("Saliendo...\n"); break;        // salgo del while(1)
        }
        else printf("Opción inválida.\n");         // validación sencilla
    }
    return 0;                                       // fin normal
}
