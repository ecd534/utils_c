/*
 * mousewrap.c
 *
 * Move o cursor do mouse continuamente pela tela (diagonal). Quando chega
 * em qualquer limite da tela (direita, esquerda, topo, base), "teleporta"
 * pro lado oposto e continua andando -- ou seja, o movimento nunca para,
 * nunca quica (bounce) e nunca fica parado esperando.
 *
 * Alem disso, a cada N segundos (5 por padrao) envia um toque da tecla
 * espaco para o Windows via SendInput -- o mesmo mecanismo de input que o
 * teclado real usa, entao o sistema (e qualquer app em foco) recebe como
 * se fosse voce apertando espaco de verdade.
 *
 * Util, por exemplo, pra manter o PC "ativo" (evitar tela de bloqueio,
 * status "ausente" no Teams/Slack etc).
 *
 * So Windows (usa a API Win32 diretamente: SetCursorPos, GetSystemMetrics,
 * SendInput).
 *
 * Compilar (com MinGW-w64 / MSYS2 UCRT64):
 *   gcc -O2 -Wall -o mousewrap.exe mousewrap.c -luser32
 *
 * Rodar:
 *   mousewrap.exe
 *   (pressione ESC a qualquer momento pra encerrar)
 *
 * Parametros opcionais de linha de comando:
 *   mousewrap.exe [passo_px] [intervalo_ms] [intervalo_espaco_s]
 *   exemplo: mousewrap.exe 8 15 10   -> passo 8px a cada 15ms, espaco a cada 10s
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

static void send_space_key(void) {
    INPUT inputs[2];
    ZeroMemory(inputs, sizeof(inputs));

    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_SPACE;
    inputs[0].ki.dwFlags = 0; /* key down */

    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_SPACE;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    UINT sent = SendInput(2, inputs, sizeof(INPUT));
    if (sent != 2) {
        fprintf(stderr, "Aviso: SendInput falhou ao enviar espaco (err=%lu)\n", GetLastError());
    }
}

int main(int argc, char **argv) {
    int step = 5;          /* pixels que o cursor anda a cada passo */
    int interval = 10;     /* milissegundos entre cada passo do mouse */
    int space_interval_s = 5; /* segundos entre cada toque de espaco */

    if (argc >= 2) step = atoi(argv[1]);
    if (argc >= 3) interval = atoi(argv[2]);
    if (argc >= 4) space_interval_s = atoi(argv[3]);
    if (step <= 0) step = 5;
    if (interval < 0) interval = 10;
    if (space_interval_s <= 0) space_interval_s = 5;

    DWORD space_interval_ms = (DWORD)space_interval_s * 1000;

    /* Usa a area virtual da tela (cobre todos os monitores, no caso de
       setup com mais de uma tela). Em setup com um monitor so, isso e
       igual a largura/altura normais da tela. */
    int screen_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int screen_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (screen_w <= 0 || screen_h <= 0) {
        fprintf(stderr, "Nao foi possivel ler as dimensoes da tela.\n");
        return 1;
    }

    POINT p;
    GetCursorPos(&p);
    int x = p.x - screen_left;
    int y = p.y - screen_top;
    if (x < 0 || x >= screen_w) x = 0;
    if (y < 0 || y >= screen_h) y = 0;

    int dx = step;
    int dy = step;

    printf("mousewrap rodando (passo=%dpx, intervalo=%dms, espaco a cada %ds).\n",
           step, interval, space_interval_s);
    printf("Area da tela: %dx%d (offset %d,%d)\n", screen_w, screen_h, screen_left, screen_top);
    printf("Pressione ESC para sair.\n");
    fflush(stdout);

    DWORD last_space_tick = GetTickCount();

    while (1) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            printf("\nESC pressionado. Saindo...\n");
            break;
        }

        x += dx;
        y += dy;

        /* Ao chegar em qualquer limite, "enrola" (wrap) pro lado oposto
           em vez de parar ou quicar. */
        if (x >= screen_w) x -= screen_w;
        if (x < 0) x += screen_w;
        if (y >= screen_h) y -= screen_h;
        if (y < 0) y += screen_h;

        SetCursorPos(screen_left + x, screen_top + y);

        DWORD now = GetTickCount();
        if (now - last_space_tick >= space_interval_ms) {
            send_space_key();
            printf("[espaco enviado]\n");
            fflush(stdout);
            last_space_tick = now;
        }

        Sleep((DWORD)interval);
    }

    return 0;
}
