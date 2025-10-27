// p2-dataProgram.c — CLIENTE para Práctica 2 SO
// Objetivo: UI de consola con menú 1..4 y “búsqueda paginada” con flechas.
// - Usa semáforo POSIX para sincronizar con el server (espera a listen())
// - Protocolo: cmd(uint32) + len(uint32) + payload bytes; respuesta idem.
// - Búsqueda paginada: "FIND|q=...|off=n", navegando con ← y → (termios modo raw)
//
// Preguntas más típicas:
// 1) ¿Por qué readn/writen? read()/write() pueden devolver parciales en sockets.
// 2) ¿Por qué htonl/ntohl? Para ser agnósticos a endianess (portabilidad).
// 3) ¿No bloquea el cliente esperando al server?  Semáforo → el server hace sem_post
//    al estar en listen(); el cliente sem_wait una vez al iniciar.
// 4) ¿Cómo capturan flechas? Con termios (ECHO/ICANON off) y leyendo secuencias ESC [ C/D.

#include "common.h"
#include <termios.h>        // para modo raw (capturar flechas sin Enter)

// Semáforo global (abierto una sola vez en main)
static sem_t *g_sem = NULL;

/* ===================  MODO RAW PARA FLECHAS  =================== */

static struct termios orig_termios;

// Restaurar modo normal de la terminal
static void disable_raw_mode(void){
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

// Activar modo raw: sin eco y sin necesidad de Enter
static int enable_raw_mode(void){
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return -1;
    atexit(disable_raw_mode);                       // garantizamos restauración
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);                // sin eco y sin modo canonico
    raw.c_cc[VMIN] = 1;                             // leer de a 1 carácter
    raw.c_cc[VTIME]= 0;                             // sin timeout
    return tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Decodificar teclas: → (1001), ← (1002), 'q' para salir, o char
static int read_key(void){
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c == 0x1b) {                                // ESC
        unsigned char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return 27;
        if (seq[0] == '[') {
            if (seq[1] == 'C') return 1001;         // Right
            if (seq[1] == 'D') return 1002;         // Left
        }
        return 27;                                  // otra secuencia ESC
    }
    return (int)c;                                  // retorno literal
}

/* ===================  CLIENTE → SERVER  =================== */

// do_request: envía comando/payload y guarda respuesta en 'out'
static int do_request(uint32_t cmd, const char *payload, char *out, size_t outsz) {
    // 1) socket TCP
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return -1; }

    // 2) dirección del server
    struct sockaddr_in srv; memset(&srv,0,sizeof(srv));
    srv.sin_family = AF_INET; srv.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &srv.sin_addr) != 1){
        perror("inet_pton"); close(sockfd); return -1;
    }

    // 3) conectar
    if (connect(sockfd,(struct sockaddr*)&srv,sizeof(srv))<0){
        perror("connect"); close(sockfd); return -1;
    }

    // 4) framing de salida
    uint32_t cmd_net = htonl(cmd);
    uint32_t len     = payload ? (uint32_t)strlen(payload) : 0;
    uint32_t len_net = htonl(len);

    if (writen(sockfd,&cmd_net,4)!=4){ perror("write cmd"); close(sockfd); return -1; }
    if (writen(sockfd,&len_net,4)!=4){ perror("write len"); close(sockfd); return -1; }
    if (len>0 && writen(sockfd,payload,len)!=(ssize_t)len){ perror("write payload"); close(sockfd); return -1; }

    // 5) framing de entrada
    uint32_t rlen_net;
    if (readn(sockfd,&rlen_net,4)!=4){ perror("read rlen"); close(sockfd); return -1; }
    uint32_t rlen = ntohl(rlen_net);
    if (rlen >= outsz) rlen = (uint32_t)outsz-1;     // truncado defensivo

    if (readn(sockfd,out,rlen)!=(ssize_t)rlen){ perror("read reply"); close(sockfd); return -1; }
    out[rlen] = '\0';

    close(sockfd);
    return 0;
}

// parse_reply: extrae RESULT/NEXT/MORE del texto del server
static void parse_reply(const char *reply, char *result, size_t rsz, long *next, int *more){
    const char *rp = strstr(reply, "RESULT:");
    const char *np = strstr(reply, "NEXT:");
    const char *mp = strstr(reply, "MORE:");
    if (rp){
        rp += 7;                                     // saltar "RESULT:"
        const char *nl = strchr(rp, '\n');
        size_t L = nl ? (size_t)(nl - rp) : strlen(rp);
        if (L >= rsz) L = rsz-1;
        memcpy(result, rp, L); result[L]='\0';
    } else { snprintf(result, rsz, "ERR: respuesta"); }
    *next = (np ? strtol(np+5, NULL, 10) : 0);
    *more = (mp ? atoi(mp+5) : 0);
}

/* ===================  UI de BÚSQUEDA PAGINADA  =================== */

// Muestra 1 resultado a la vez y permite navegar con ← y →, o 'q' para salir
static void search_interactive(const char *q){
    long off = 0;                                    // offset actual (0 = primera)
    for (;;) {
        char payload[1024], reply[MAX_REPLY], result_line[1024];
        snprintf(payload, sizeof(payload), "FIND|q=%s|off=%ld", q, off);

        if (do_request(1, payload, reply, sizeof(reply)) != 0){
            fprintf(stderr, "Error consultando.\n");
            return;
        }
        long next=off; int more=0;
        parse_reply(reply, result_line, sizeof(result_line), &next, &more);

        printf("\n[Resultado #%ld]\n%s", off + 1 , result_line); // RESULT ya trae \n del CSV
        printf("[← anterior | → siguiente | q salir]\n");

        if (enable_raw_mode() == -1) { perror("raw mode"); return; }
        int k = read_key();                            // leer una tecla sin Enter
        disable_raw_mode();

        if (k=='q' || k=='Q' || k==27) break;         // ESC o q = salir
        if (k==1001) {                                 // → siguiente
            if (more) off = next;
            else printf("(no hay más)\n");
        } else if (k==1002) {                          // ← anterior
            if (off>0) off -= 1;
            else printf("(ya estás en el primero)\n");
        } else {
            // ignoramos otras teclas
        }
    }
}

// prompt_line simple para leer del usuario con fgets
static void prompt_line(const char *msg, char *out, size_t n){
    printf("%s", msg); fflush(stdout);
    if (!fgets(out,n,stdin)) { out[0]='\0'; return; }
    rstrip_newline(out);
}

/* ===================  MAIN (menú 1..4)  =================== */

int main(void){
    // Sincronizar UNA sola vez con el servidor:
    // el server hace sem_post cuando ya está en listen(), el cliente espera aquí.
    g_sem = sem_open(SEM_NAME, 0);
    if (g_sem == SEM_FAILED){
        perror("[client] sem_open");
        fprintf(stderr, "Arranca primero el servidor ./p2-searchd\n");
        return 1;
    }
    if (sem_wait(g_sem) < 0){
        perror("[client] sem_wait");
        return 1;
    }

    // Menú principal
    for (;;) {
        printf("\nBienvenido\n");
        printf("1) Realizar búsqueda (paginada con flechas)\n");
        printf("2) Escribir un registro\n");
        printf("3) Leer a partir de número de registro\n");
        printf("4) Salir\n");
        printf("Seleccione opción [1-4]: ");

        int opt = read_menu_option();
        if (opt < 0) { printf("Opción inválida.\n"); continue; }
        if (opt == 4) { printf("Saliendo...\n"); break; }

        if (opt == 1){
            char q[MAX_LINE];
            prompt_line("Ingrese cadena a buscar: ", q, sizeof(q));
            if (!*q){ printf("Entrada vacía.\n"); continue; }
            search_interactive(q);                    // ← navegación con flechas

        } else if (opt == 2){
            char rec[MAX_LINE], reply[MAX_REPLY], line[1024];
            prompt_line("Ingrese registro CSV a guardar: ", rec, sizeof(rec));
            if (!*rec){ printf("Entrada vacía.\n"); continue; }
            if (do_request(2, rec, reply, sizeof(reply))==0) {
                long nxt; int mr; parse_reply(reply, line, sizeof(line), &nxt, &mr);
                printf("%s\n", line);                // debería imprimir "OK saved"
            }

        } else if (opt == 3){
            char numbuf[64], reply[MAX_REPLY], line[1024];
            prompt_line("Número de registro: ", numbuf, sizeof(numbuf));
            bool ok=true; for(size_t i=0;i<strlen(numbuf);++i) if(!isdigit((unsigned char)numbuf[i])) ok=false;
            if (!ok || !*numbuf){ printf("Valor inválido.\n"); continue; }
            char payload[128]; snprintf(payload,sizeof(payload),"READIDX:%s",numbuf);
            if (do_request(1, payload, reply, sizeof(reply))==0){
                long nxt; int mr; parse_reply(reply, line, sizeof(line), &nxt, &mr);
                printf("%s\n", line);                // línea #n del CSV (o NA)
            }
        }
    }

    if (g_sem) sem_close(g_sem);                      // cerrar semáforo (no unlink)
    return 0;
}
