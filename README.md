# dirspace

Analisador de espaço em disco em modo texto (terminal), escrito em C puro,
que roda tanto no Windows quanto no Linux a partir do mesmo `dirspace.c`.

Mostra, nível a nível, o tamanho de cada subpasta/arquivo do diretório atual
com uma barra de percentual (estilo WinDirStat, mas em texto), e deixa você
"entrar" em uma subpasta para ver o próximo nível.

## Compilar

**Linux:**
```
gcc -O2 -Wall -o dirspace dirspace.c
```

**Windows (MinGW-w64):**
```
gcc -O2 -Wall -o dirspace.exe dirspace.c
```
(compilado e testado aqui via cross-compile com `x86_64-w64-mingw32-gcc`)

Não usa nenhuma biblioteca externa — só a biblioteca padrão do C e as APIs
nativas do sistema operacional (`windows.h` no Windows; `dirent.h` /
`sys/statvfs.h` no Linux).

## Usar

```
./dirspace
```
Mostra a lista de unidades (Windows: C:\, D:\, ...) ou dos pontos de
montagem reais (Linux: /, /home, /boot, discos externos etc. — filesystems
"pseudo" como proc/sysfs/tmpfs são filtrados automaticamente). Escolha um
número ou digite um caminho manualmente.

```
./dirspace /algum/caminho
```
Pula a seleção de unidade e já abre direto nesse caminho.

Dentro da navegação:
- `[número]` entra na pasta correspondente
- `v` volta um nível
- `a` atualiza (recalcula) o nível atual
- `q` sai do programa

Para pastas grandes o cálculo recursivo do tamanho pode demorar; durante o
cálculo aparece uma contagem de itens escaneados no rodapé.

## Limitações conhecidas

- Links simbólicos não são seguidos (evita loops infinitos e contagem
  duplicada) — aparecem listados com tamanho 0.
- Pastas sem permissão de leitura são marcadas como "(sem acesso)".
- Cada vez que você entra em uma pasta o tamanho dela é recalculado do zero
  (não há cache) — é simples de propósito, mas em discos muito grandes pode
  ser lento.
