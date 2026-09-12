/*
 * dirspace.c
 *
 * Analisador de espaco em disco por pasta, em modo texto, com barra de
 * percentual estilo WinDirStat. Navega nivel a nivel: mostra o tamanho de
 * cada subpasta/arquivo do diretorio atual e permite "entrar" em uma
 * subpasta para ver o proximo nivel.
 *
 * Multiplataforma: compila tanto para Windows quanto para Linux a partir
 * do mesmo arquivo-fonte, usando #ifdef _WIN32 para isolar as chamadas de
 * sistema especificas de cada SO.
 *
 * Compilar no Linux:
 *   gcc -O2 -Wall -o dirspace dirspace.c
 *
 * Compilar no Windows (MinGW):
 *   gcc -O2 -Wall -o dirspace.exe dirspace.c
 *
 * Uso:
 *   ./dirspace              -> mostra a tela de selecao de unidade/disco
 *   ./dirspace /algum/caminho   -> comeca a navegacao direto nesse caminho
 */

#ifndef _WIN32
    /* Garante que lstat(), etc. fiquem declarados por dirent.h/sys/stat.h
       em sistemas com glibc estrita. Precisa vir antes de qualquer include. */
    #define _POSIX_C_SOURCE 200809L
    #define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
    #include <windows.h>
    #define PATH_SEP '\\'
    #define PATH_SEP_STR "\\"
#else
    #include <dirent.h>
    #include <sys/stat.h>
    #include <sys/statvfs.h>
    #include <unistd.h>
    #define PATH_SEP '/'
    #define PATH_SEP_STR "/"
#endif

#define MAX_PATH_LEN 4096
#define MAX_NAME_LEN 512
#define MAX_ENTRIES  8192
#define MAX_DEPTH    256
#define BAR_WIDTH    30

/* ------------------------------------------------------------------ */
/* Estruturas                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[MAX_NAME_LEN];
    char fullpath[MAX_PATH_LEN];
    unsigned long long size;
    int is_dir;
    int access_denied;
} Entry;

static unsigned long g_scanned_files = 0;
static unsigned long g_progress_counter = 0;

/* ------------------------------------------------------------------ */
/* Utilitarios de formatacao                                          */
/* ------------------------------------------------------------------ */

static void format_size(unsigned long long bytes, char *buf, size_t bufsize) {
    const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
    double val = (double)bytes;
    int unit = 0;
    while (val >= 1024.0 && unit < 5) {
        val /= 1024.0;
        unit++;
    }
    if (unit == 0)
        snprintf(buf, bufsize, "%.0f %s", val, units[unit]);
    else
        snprintf(buf, bufsize, "%.2f %s", val, units[unit]);
}

static void draw_bar(char *buf, size_t bufsize, double percent, int width) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int filled = (int)(percent / 100.0 * width + 0.5);
    if (filled > width) filled = width;
    char bar[BAR_WIDTH + 1];
    int i;
    for (i = 0; i < filled; i++) bar[i] = '#';
    for (; i < width; i++) bar[i] = '-';
    bar[width] = '\0';
    snprintf(buf, bufsize, "[%s] %5.1f%%", bar, percent);
}

static void print_progress(const char *current_name) {
    g_progress_counter++;
    if (g_progress_counter % 250 == 0) {
        fprintf(stderr, "\rEscaneando... %lu itens (%.40s)          ",
                g_scanned_files, current_name ? current_name : "");
        fflush(stderr);
    }
}

static void clear_progress_line(void) {
    fprintf(stderr, "\r%80s\r", "");
    fflush(stderr);
}

static void join_path(char *out, size_t outsize, const char *base, const char *name) {
    size_t len = strlen(base);
    if (len > 0 && base[len - 1] == PATH_SEP)
        snprintf(out, outsize, "%s%s", base, name);
    else
        snprintf(out, outsize, "%s%c%s", base, PATH_SEP, name);
}

/* ------------------------------------------------------------------ */
/* Calculo recursivo de tamanho de diretorio                          */
/* ------------------------------------------------------------------ */

#ifdef _WIN32

static unsigned long long dir_size_recursive(const char *path) {
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;

    unsigned long long total = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        g_scanned_files++;
        print_progress(fd.cFileName);

        char child[MAX_PATH_LEN];
        join_path(child, sizeof(child), path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            /* Nao segue links simbolicos / junctions para evitar loops. */
            continue;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += dir_size_recursive(child);
        } else {
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            total += sz.QuadPart;
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return total;
}

static int list_immediate_entries(const char *path, Entry *entries, int max_entries) {
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;

    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (count >= max_entries)
            break;

        Entry *e = &entries[count];
        strncpy(e->name, fd.cFileName, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        join_path(e->fullpath, sizeof(e->fullpath), path, fd.cFileName);
        e->access_denied = 0;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            e->is_dir = 0;
            e->size = 0;
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            e->is_dir = 1;
            g_scanned_files++;
            print_progress(e->name);
            e->size = dir_size_recursive(e->fullpath);
        } else {
            e->is_dir = 0;
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            e->size = sz.QuadPart;
        }
        count++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return count;
}

static void get_disk_usage(const char *path, unsigned long long *total, unsigned long long *free_) {
    ULARGE_INTEGER freeAvail, totalBytes, totalFree;
    if (GetDiskFreeSpaceExA(path, &freeAvail, &totalBytes, &totalFree)) {
        *total = totalBytes.QuadPart;
        *free_ = totalFree.QuadPart;
    } else {
        *total = 0;
        *free_ = 0;
    }
}

static int list_drives(char drives[][8], int max_drives) {
    DWORD mask = GetLogicalDrives();
    int count = 0;
    for (int i = 0; i < 26 && count < max_drives; i++) {
        if (mask & (1u << i)) {
            char root[8];
            snprintf(root, sizeof(root), "%c:\\", 'A' + i);
            UINT type = GetDriveTypeA(root);
            if (type == DRIVE_NO_ROOT_DIR)
                continue;
            snprintf(drives[count], sizeof(drives[count]), "%s", root);
            count++;
        }
    }
    return count;
}

#else /* POSIX (Linux) */

static unsigned long long dir_size_recursive(const char *path) {
    DIR *d = opendir(path);
    if (!d)
        return 0;

    unsigned long long total = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        g_scanned_files++;
        print_progress(entry->d_name);

        char child[MAX_PATH_LEN];
        join_path(child, sizeof(child), path, entry->d_name);

        struct stat st;
        if (lstat(child, &st) != 0)
            continue;

        if (S_ISLNK(st.st_mode)) {
            /* Nao segue links simbolicos, evita loops e contagem duplicada. */
            continue;
        } else if (S_ISDIR(st.st_mode)) {
            total += dir_size_recursive(child);
        } else if (S_ISREG(st.st_mode)) {
            total += (unsigned long long)st.st_size;
        }
    }
    closedir(d);
    return total;
}

static int list_immediate_entries(const char *path, Entry *entries, int max_entries) {
    DIR *d = opendir(path);
    if (!d)
        return -1;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (count >= max_entries)
            break;

        Entry *e = &entries[count];
        strncpy(e->name, entry->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        join_path(e->fullpath, sizeof(e->fullpath), path, entry->d_name);
        e->access_denied = 0;

        struct stat st;
        if (lstat(e->fullpath, &st) != 0) {
            e->is_dir = 0;
            e->size = 0;
            e->access_denied = 1;
            count++;
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            e->is_dir = 0;
            e->size = 0;
        } else if (S_ISDIR(st.st_mode)) {
            e->is_dir = 1;
            g_scanned_files++;
            print_progress(e->name);
            e->size = dir_size_recursive(e->fullpath);
        } else {
            e->is_dir = 0;
            e->size = (unsigned long long)st.st_size;
        }
        count++;
    }
    closedir(d);
    return count;
}

static void get_disk_usage(const char *path, unsigned long long *total, unsigned long long *free_) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) == 0) {
        *total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
        *free_ = (unsigned long long)vfs.f_bfree * vfs.f_frsize;
    } else {
        *total = 0;
        *free_ = 0;
    }
}

/* Filesystems "pseudo" que normalmente nao interessam ao usuario. */
static int is_ignorable_fstype(const char *fstype) {
    static const char *ignored[] = {
        "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "cgroup", "cgroup2",
        "pstore", "debugfs", "securityfs", "mqueue", "hugetlbfs", "tracefs",
        "configfs", "fusectl", "binfmt_misc", "autofs", "rpc_pipefs",
        "overlay", "squashfs", "efivarfs", "bpf", NULL
    };
    for (int i = 0; ignored[i]; i++)
        if (strcmp(fstype, ignored[i]) == 0)
            return 1;
    return 0;
}

static int list_mount_points(char mounts[][256], int max_mounts) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return 0;

    char line[1024];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < max_mounts) {
        char device[256], mountpoint[256], fstype[64];
        if (sscanf(line, "%255s %255s %63s", device, mountpoint, fstype) != 3)
            continue;
        if (is_ignorable_fstype(fstype))
            continue;
        snprintf(mounts[count], 256, "%s", mountpoint);
        count++;
    }
    fclose(f);
    return count;
}

#endif

/* ------------------------------------------------------------------ */
/* Ordenacao                                                          */
/* ------------------------------------------------------------------ */

static int compare_entries_desc(const void *a, const void *b) {
    const Entry *ea = (const Entry *)a;
    const Entry *eb = (const Entry *)b;
    if (eb->size > ea->size) return 1;
    if (eb->size < ea->size) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tela / navegacao                                                    */
/* ------------------------------------------------------------------ */

static void clear_screen(void) {
    /* Sequencia ANSI, suportada pelo terminal do Linux e pelo Windows
       Terminal / cmd.exe moderno (Windows 10+). */
    printf("\033[2J\033[H");
}

static void read_line(char *buf, size_t bufsize) {
    if (fgets(buf, (int)bufsize, stdin) == NULL) {
        buf[0] = '\0';
        return;
    }
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[len - 1] = '\0';
        len--;
    }
}

/* Mostra o conteudo de "path" (um nivel), permite navegar para dentro de
   subpastas, e retorna quando o usuario pede para voltar ou sair.
   Retorna 0 para "voltar" (sair desta pasta), 1 para "sair do programa". */
static int browse_level(char stack[][MAX_PATH_LEN], int *depth) {
    static Entry entries[MAX_ENTRIES];
    char input[128];

    while (1) {
        const char *path = stack[*depth - 1];

        g_scanned_files = 0;
        g_progress_counter = 0;
        printf("Calculando tamanhos em: %s\n", path);
        fflush(stdout);

        int count = list_immediate_entries(path, entries, MAX_ENTRIES);
        clear_progress_line();

        if (count < 0) {
            printf("\nNao foi possivel abrir esta pasta (permissao negada ou caminho invalido).\n");
            printf("Pressione ENTER para voltar...");
            read_line(input, sizeof(input));
            return 0;
        }

        qsort(entries, count, sizeof(Entry), compare_entries_desc);

        unsigned long long level_total = 0;
        for (int i = 0; i < count; i++)
            level_total += entries[i].size;

        unsigned long long disk_total = 0, disk_free = 0;
        get_disk_usage(path, &disk_total, &disk_free);

        clear_screen();
        printf("========================================================================\n");
        printf(" Pasta atual: %s\n", path);
        if (disk_total > 0) {
            unsigned long long disk_used = disk_total - disk_free;
            char tbuf[32], ubuf[32], fbuf[32], barbuf[64];
            format_size(disk_total, tbuf, sizeof(tbuf));
            format_size(disk_used, ubuf, sizeof(ubuf));
            format_size(disk_free, fbuf, sizeof(fbuf));
            double pct = disk_total > 0 ? (100.0 * (double)disk_used / (double)disk_total) : 0.0;
            draw_bar(barbuf, sizeof(barbuf), pct, BAR_WIDTH);
            printf(" Disco: %s usados de %s (livre: %s)\n", ubuf, tbuf, fbuf);
            printf(" %s\n", barbuf);
        }
        printf("========================================================================\n");
        printf(" %-3s %-38s %-10s %-6s %s\n", "#", "NOME", "TAMANHO", "TIPO", "% DESTA PASTA");
        printf("------------------------------------------------------------------------\n");

        if (count == 0) {
            printf(" (pasta vazia)\n");
        }

        for (int i = 0; i < count; i++) {
            char sbuf[32], barbuf[64];
            format_size(entries[i].size, sbuf, sizeof(sbuf));
            double pct = level_total > 0 ? (100.0 * (double)entries[i].size / (double)level_total) : 0.0;
            draw_bar(barbuf, sizeof(barbuf), pct, 20);

            char display_name[64];
            snprintf(display_name, sizeof(display_name), "%.38s%s",
                     entries[i].name, entries[i].access_denied ? " (sem acesso)" : "");

            printf(" %-3d %-38s %-10s %-6s %s\n",
                   i + 1,
                   display_name,
                   sbuf,
                   entries[i].is_dir ? "DIR" : "arq",
                   barbuf);
        }

        printf("------------------------------------------------------------------------\n");
        printf(" [numero] entrar na pasta   [v] voltar   [a] atualizar   [q] sair\n");
        printf("> ");
        fflush(stdout);

        read_line(input, sizeof(input));

        if (input[0] == '\0')
            continue;

        if (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0) {
            return 1;
        } else if (strcmp(input, "v") == 0 || strcmp(input, "V") == 0) {
            return 0;
        } else if (strcmp(input, "a") == 0 || strcmp(input, "A") == 0) {
            continue;
        } else {
            char *endptr;
            long idx = strtol(input, &endptr, 10);
            if (*endptr != '\0' || idx < 1 || idx > count) {
                printf("Opcao invalida. Pressione ENTER para continuar...");
                read_line(input, sizeof(input));
                continue;
            }
            Entry *chosen = &entries[idx - 1];
            if (!chosen->is_dir) {
                printf("'%s' e um arquivo, nao uma pasta. Pressione ENTER para continuar...", chosen->name);
                read_line(input, sizeof(input));
                continue;
            }
            if (chosen->access_denied) {
                printf("Sem permissao para acessar '%s'. Pressione ENTER para continuar...", chosen->name);
                read_line(input, sizeof(input));
                continue;
            }
            if (*depth >= MAX_DEPTH) {
                printf("Profundidade maxima de navegacao atingida. Pressione ENTER...");
                read_line(input, sizeof(input));
                continue;
            }
            strncpy(stack[*depth], chosen->fullpath, MAX_PATH_LEN - 1);
            stack[*depth][MAX_PATH_LEN - 1] = '\0';
            (*depth)++;
            int result = browse_level(stack, depth);
            (*depth)--;
            if (result == 1)
                return 1;
            /* result == 0 -> voltamos para este nivel, continua o loop */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Selecao inicial de unidade / disco                                 */
/* ------------------------------------------------------------------ */

static void run_root_selection(void) {
    char input[128];

#ifdef _WIN32
    char drives[26][8];
    int count = list_drives(drives, 26);
#else
    char drives[64][256];
    int count = list_mount_points(drives, 64);
#endif

    while (1) {
        clear_screen();
        printf("========================================================================\n");
        printf(" dirspace - analisador de espaco em disco\n");
        printf("========================================================================\n");
        if (count == 0) {
            printf(" Nenhuma unidade/particao detectada automaticamente.\n");
        } else {
            printf(" Unidades/particoes disponiveis:\n\n");
            for (int i = 0; i < count; i++) {
                unsigned long long total = 0, free_ = 0;
                get_disk_usage(drives[i], &total, &free_);
                if (total > 0) {
                    unsigned long long used = total - free_;
                    char tbuf[32], ubuf[32];
                    format_size(total, tbuf, sizeof(tbuf));
                    format_size(used, ubuf, sizeof(ubuf));
                    printf("   [%d] %-30s  %s usados de %s\n", i + 1, drives[i], ubuf, tbuf);
                } else {
                    printf("   [%d] %-30s  (nao foi possivel ler)\n", i + 1, drives[i]);
                }
            }
        }
        printf("\n Digite o numero de uma unidade, ou digite um caminho manualmente.\n");
        printf(" [q] sair\n");
        printf("> ");
        fflush(stdout);

        read_line(input, sizeof(input));
        if (input[0] == '\0')
            continue;
        if (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0)
            return;

        char chosen_path[MAX_PATH_LEN];
        char *endptr;
        long idx = strtol(input, &endptr, 10);
        if (*endptr == '\0' && idx >= 1 && idx <= count) {
            strncpy(chosen_path, drives[idx - 1], sizeof(chosen_path) - 1);
            chosen_path[sizeof(chosen_path) - 1] = '\0';
        } else {
            strncpy(chosen_path, input, sizeof(chosen_path) - 1);
            chosen_path[sizeof(chosen_path) - 1] = '\0';
        }

        char stack[MAX_DEPTH][MAX_PATH_LEN];
        int depth = 1;
        strncpy(stack[0], chosen_path, MAX_PATH_LEN - 1);
        stack[0][MAX_PATH_LEN - 1] = '\0';

        int result = browse_level(stack, &depth);
        if (result == 1)
            return;
        /* senao, volta para a tela de selecao de unidade */
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    if (argc >= 2) {
        char stack[MAX_DEPTH][MAX_PATH_LEN];
        int depth = 1;
        strncpy(stack[0], argv[1], MAX_PATH_LEN - 1);
        stack[0][MAX_PATH_LEN - 1] = '\0';
        browse_level(stack, &depth);
    } else {
        run_root_selection();
    }

    printf("\nAte mais!\n");
    return 0;
}
