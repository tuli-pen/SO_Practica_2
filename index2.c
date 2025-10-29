#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "index.h"
#include "hash.h"

#define RANGE 12  // rango para búsqueda parcial

void trim_newline(char *str) {
    if (!str) return;
    size_t len = strlen(str);
    if (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r'))
        str[len - 1] = '\0';
}


// --- Función para limpiar texto ---
void limpiar_texto(char *s) {
    if (!s) return;
    size_t len = strlen(s);
    if (len == 0) return;

    if (s[0] == '"') memmove(s, s + 1, len--);
    if (len > 0 && s[len - 1] == '"') s[len - 1] = '\0';

    len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[len - 1] = '\0';
        len--;
    }
}

// --- Función que construye el índice si no existe ---
int build_index(const char *csv_path, const char *index_path) {
    FILE *csv = fopen(csv_path, "r");
    if (!csv) { perror("Error abriendo CSV"); return -1; }

    FILE *idx = fopen(index_path, "wb+");
    if (!idx) { perror("Error creando índice"); fclose(csv); return -1; }

    // --- Header ---
    IndexHeader header = { N_BUCKETS, sizeof(IndexHeader), sizeof(IndexHeader) + sizeof(BucketDisk) * N_BUCKETS };
    fwrite(&header, sizeof(IndexHeader), 1, idx);

    // --- Buckets vacíos ---
    BucketDisk empty = { .first_entry_offset = -1 };
    for (int i = 0; i < N_BUCKETS; i++)
        fwrite(&empty, sizeof(BucketDisk), 1, idx);

    // --- Leer CSV ---
    char line[4096];
    long line_start;
    fgets(line, sizeof(line), csv); // saltar encabezado

    while ((line_start = ftell(csv)), fgets(line, sizeof(line), csv)) {
        char *token = strtok(line, ",\n\r");
        char *key = NULL;
        int col = 1;
        while (token) {
            if (col == 4) { key = token; break; }
            token = strtok(NULL, ",\n\r");
            col++;
        }
        if (!key) continue;
        limpiar_texto(key);

        unsigned long h = hash_string(key) % N_BUCKETS;
        long bucket_offset = sizeof(IndexHeader) + sizeof(BucketDisk) * h;

        BucketDisk b;
        fseek(idx, bucket_offset, SEEK_SET);
        fread(&b, sizeof(BucketDisk), 1, idx);

        EntryDisk entry = {0};
        strncpy(entry.key, key, KEY_SIZE - 1);
        entry.csv_offset = line_start;
        entry.next_entry = b.first_entry_offset;

        fseek(idx, 0, SEEK_END);
        long new_entry_offset = ftell(idx);
        fwrite(&entry, sizeof(EntryDisk), 1, idx);

        b.first_entry_offset = new_entry_offset;
        fseek(idx, bucket_offset, SEEK_SET);
        fwrite(&b, sizeof(BucketDisk), 1, idx);
    }

    fclose(csv);
    fclose(idx);
    printf("Índice generado correctamente con %d buckets.\n", N_BUCKETS);
    return 0;
}

// --- Función de búsqueda híbrida ---
void append_and_reindex_bin(
    const char *csv_path,
    const char *id,
    const char *submitter,
    const char *authors,
    const char *title,
    const char *abstract,
    const char *categories,
    const char *comments,
    const char *journal_ref,
    const char *doi,
    const char *report_no,
    const char *license,
    const char *update_date,
    const char *versions_count,
    const char *versions_last_created
) {
    // 1️⃣ Abrir CSV para añadir al final
    FILE *fcsv = fopen(csv_path, "a+b");
    if (!fcsv) {
        perror("No se pudo abrir o crear el CSV");
        return;
    }

    fseek(fcsv, 0, SEEK_END);
    long csv_offset = ftell(fcsv);

    // 2️⃣ Escribir nueva línea en formato CSV
    char line[4096];
    snprintf(line, sizeof(line),
        "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
        id, submitter, authors, title, abstract, categories, comments,
        journal_ref, doi, report_no, license, update_date, versions_count, versions_last_created
    );

    fwrite(line, 1, strlen(line), fcsv);
    fclose(fcsv);
    printf("[CSV] Nuevo registro añadido (offset=%ld)\n", csv_offset);

    // 3️⃣ Abrir índice binario
    FILE *findex = fopen("index.bin", "r+b");
    if (!findex) {
        perror("No se pudo abrir index.bin");
        return;
    }

    // 4️⃣ Leer header
    IndexHeader header;
    fread(&header, sizeof(IndexHeader), 1, findex);

    // 5️⃣ Calcular bucket
    unsigned long h = hash_string(title);
    int bucket_id = h % header.n_buckets;
    long bucket_offset = header.offset_buckets + bucket_id * sizeof(BucketDisk);

    // 6️⃣ Leer bucket existente
    fseek(findex, bucket_offset, SEEK_SET);
    BucketDisk bucket;
    fread(&bucket, sizeof(BucketDisk), 1, findex);

    // 7️⃣ Crear nueva entrada
    EntryDisk entry;
    memset(&entry, 0, sizeof(EntryDisk));
    strncpy(entry.key, title, KEY_SIZE - 1);
    entry.csv_offset = csv_offset;
    entry.next_entry = bucket.first_entry_offset;

    // 8️⃣ Guardar nueva entrada al final
    fseek(findex, 0, SEEK_END);
    long new_entry_offset = ftell(findex);
    fwrite(&entry, sizeof(EntryDisk), 1, findex);

    // 9️⃣ Actualizar bucket
    bucket.first_entry_offset = new_entry_offset;
    fseek(findex, bucket_offset, SEEK_SET);
    fwrite(&bucket, sizeof(BucketDisk), 1, findex);

    fclose(findex);
    printf("[INDEX] '%s' insertado en bucket %d (entry_offset=%ld)\n",
           title, bucket_id, new_entry_offset);
}

void search_by_keyword2(const char *keyword, int exact, const char *index_file) {
    if (!keyword) return;

    FILE *idx = fopen(index_file, "rb");
    FILE *csv = fopen("arxiv.csv", "r");
    if (!idx || !csv) {
        perror("Error abriendo archivos");
        if (idx) fclose(idx);
        if (csv) fclose(csv);
        return;
    }

    clock_t start = clock();
    unsigned long h = hash_string(keyword);
    IndexHeader header;
    fread(&header, sizeof(IndexHeader), 1, idx);

    int bucket_id = h % header.n_buckets;
    long bucket_offset = header.offset_buckets + bucket_id * sizeof(BucketDisk);

    fseek(idx, bucket_offset, SEEK_SET);
    BucketDisk b;
    fread(&b, sizeof(BucketDisk), 1, idx);

    long current = b.first_entry_offset;
    EntryDisk entry;
    int found = 0;

    while (current != -1) {
        fseek(idx, current, SEEK_SET);
        fread(&entry, sizeof(EntryDisk), 1, idx);

        int match = 0;
        if (exact) {
            if (strcasecmp(entry.key, keyword) == 0) match = 1;
        } else {
            if (strcasestr(entry.key, keyword)) match = 1;
        }

        if (match) {
            found++;
            fseek(csv, entry.csv_offset, SEEK_SET);
            char line[4096];
            if (fgets(line, sizeof(line), csv))
                printf("%s", line);

            if (found >= 50) {
                printf("\nMostrando solo las primeras 50 coincidencias.\n");
                break;
            }
        }

        current = entry.next_entry;
    }

    double seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
    if (!found)
        printf("No se encontraron resultados.\n");
    else
        printf("\n%d resultado(s) encontrado(s) en %.3f segundos.\n", found, seconds);

    fclose(idx);
    fclose(csv);
}

int main2() {
    int opcion;
    char csv_path[] = "arxiv.csv";
    char index_path[] = "index.bin";

    do {
        printf("\n==============================\n");
        printf("   🧩 MENÚ PRINCIPAL INDEXADOR\n");
        printf("==============================\n");
        printf("1. Insertar nuevo registro\n");
        printf("2. Buscar por título\n");
        printf("3. Salir\n");
        printf("Seleccione una opción: ");
        scanf("%d", &opcion);
        getchar(); // limpiar \n

        if (opcion == 1) {
            char id[64], submitter[128], authors[256], title[256], abstract[512];
            char categories[128], comments[128], journal_ref[128], doi[128];
            char report_no[64], license[64], update_date[64], versions_count[16], versions_last_created[64];

            printf("\nIngrese los datos del nuevo registro:\n");
            printf("ID: "); fgets(id, sizeof(id), stdin); trim_newline(id);
            printf("Submitter: "); fgets(submitter, sizeof(submitter), stdin); trim_newline(submitter);
            printf("Authors: "); fgets(authors, sizeof(authors), stdin); trim_newline(authors);
            printf("Title: "); fgets(title, sizeof(title), stdin); trim_newline(title);
            printf("Abstract: "); fgets(abstract, sizeof(abstract), stdin); trim_newline(abstract);
            printf("Categories: "); fgets(categories, sizeof(categories), stdin); trim_newline(categories);
            printf("Comments: "); fgets(comments, sizeof(comments), stdin); trim_newline(comments);
            printf("Journal-ref: "); fgets(journal_ref, sizeof(journal_ref), stdin); trim_newline(journal_ref);
            printf("DOI: "); fgets(doi, sizeof(doi), stdin); trim_newline(doi);
            printf("Report-no: "); fgets(report_no, sizeof(report_no), stdin); trim_newline(report_no);
            printf("License: "); fgets(license, sizeof(license), stdin); trim_newline(license);
            printf("Update date: "); fgets(update_date, sizeof(update_date), stdin); trim_newline(update_date);
            printf("Versions count: "); fgets(versions_count, sizeof(versions_count), stdin); trim_newline(versions_count);
            printf("Versions last created: "); fgets(versions_last_created, sizeof(versions_last_created), stdin); trim_newline(versions_last_created);

            append_and_reindex_bin(csv_path, id, submitter, authors, title, abstract, categories,
                                   comments, journal_ref, doi, report_no, license, update_date,
                                   versions_count, versions_last_created);

        } else if (opcion == 2) {
            char keyword[256];
            int exact;
            printf("\nIngrese palabra clave a buscar: ");
            fgets(keyword, sizeof(keyword), stdin);
            trim_newline(keyword);
            printf("¿Búsqueda exacta? (1 = Sí, 0 = No): ");
            scanf("%d", &exact);
            getchar();
            search_by_keyword2(keyword, exact, index_path);

        } else if (opcion == 3) {
            printf("Saliendo...\n");
        } else {
            printf("Opción no válida.\n");
        }

    } while (opcion != 3);

    return 0;
}
