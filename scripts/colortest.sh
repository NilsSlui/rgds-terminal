#!/bin/sh
# Prints text styles, the 16/256-colour palettes, a truecolor gradient and some
# Unicode, to check the terminal's rendering. Run it on the device.
e=$(printf '\033')

printf '%s[1mText styles%s[0m   %s[1mbold%s[0m  %s[3mitalic%s[0m  %s[4munderline%s[0m  ' \
    "$e" "$e" "$e" "$e" "$e" "$e" "$e" "$e"
printf '%s[9mstrike%s[0m  %s[7mreverse%s[0m\n\n' "$e" "$e" "$e" "$e"

printf '%s[1m16 colours%s[0m\n' "$e" "$e"
for i in 0 1 2 3 4 5 6 7; do printf '%s[4%sm    ' "$e" "$i"; done
printf '%s[0m\n' "$e"
for i in 0 1 2 3 4 5 6 7; do printf '%s[10%sm    ' "$e" "$i"; done
printf '%s[0m\n\n' "$e"

printf '%s[1m256 colours%s[0m\n' "$e" "$e"
i=16
while [ $i -le 231 ]; do
    printf '%s[48;5;%sm ' "$e" "$i"
    [ $(( (i - 15) % 72 )) -eq 0 ] && printf '%s[0m\n' "$e"
    i=$((i + 1))
done
i=232
while [ $i -le 255 ]; do printf '%s[48;5;%sm   ' "$e" "$i"; i=$((i + 1)); done
printf '%s[0m\n\n' "$e"

printf '%s[1mTruecolor%s[0m\n' "$e" "$e"
i=0
while [ $i -lt 72 ]; do
    printf '%s[48;2;%s;%s;%sm ' "$e" $((i * 255 / 71)) $((180 - i * 2)) $((255 - i * 255 / 71))
    i=$((i + 1))
done
printf '%s[0m\n\n' "$e"

printf '%s[1mUnicode%s[0m   ┌──────┐ ░▒▓█ → ✓ ★ π ≠ ±  中文 日本語 한국어\n' "$e" "$e"
printf '          └──────┘\n'
