<div align="center">
MP3PLUS Reproductor de Audio para M5Stack Cardputer-Adv y Cardputer

🇬🇧 English | 🇷🇺 Русский | 🇪🇸 Español

License: GPL v3

Hecho para quienes realmente llevan toda su biblioteca musical en el bolsillo — sin PSRAM, sin lag y sin excusas.

¿Por qué otro reproductor?
La mayoría de los reproductores para Cardputer se quedan cortos en 256–2.000 pistas, fallan con nombres UTF-8 o se congelan al activar el modo aleatorio. MP3+ exprime al máximo los 320 KB de SRAM utilizables del ESP32-S3:

Hasta 4.096 pistas por carpeta — índice ligero en microSD (/.mp3plus.idx) + caché rápida en RAM de 16 entradas. Probado en hardware real con ~1.000 canciones en una sola carpeta sin tirones.

Shuffle instantáneo en 0 ms (S) — permutación Fisher-Yates no destructiva en RAM. Actívalo para mezclar; desactívalo para volver al orden original exacto sin tocar tus archivos.

Formatos: MP3, M4A (AAC), WAV — decodificación fluida en doble núcleo (Core 0) sin necesidad de PSRAM.

Amp Boost por hardware (A) — ganancia directa de 100% / 150% (+6 dB) / 200% (+11 dB) en el DAC ES8311 del Cardputer-Adv, más conmutación automática en el jack de 3.5 mm.

Compatibilidad 2-en-1 — detecta automáticamente M5Stack Cardputer-Adv y M5Stack Cardputer original desde un único .bin.

Visualizador de 5 zonas con AGC (V) — DSP real de 3 bandas con control automático de ganancia: las barras se mueven con energía incluso a volumen bajo (03). Modos: BARS, PEAK, WAVE, OFF.

Soporte UTF-8 / Cirílico completo — títulos limpios en 3 tamaños de fuente (G).

Modo Bolsillo (botón lateral G0) — controla la música con la pantalla apagada: 1× Play/Pausa, 2× Siguiente, 3× Anterior.

Pensado para la batería — 6 temas de color (T), brillo limpio en pasos de 20% a 100%, reposo de pantalla, modo CPU Eco de 160 MHz y guardado automático en NVS.

Instalación rápida (M5Launcher)
Descarga el último .bin desde [suspicious link removed].

Cópialo en tu tarjeta microSD en FAT32 (por ejemplo, /MP3PLUS.bin).

Abre M5Launcher en tu Cardputer, elige el .bin y pulsa Install (o Run).

Pon tus carpetas de música donde quieras en la SD y pulsa F para explorar.

Controles
Tecla	Acción
Enter / Space	Play / Pausa / Abrir carpeta (Space reproduce toda la carpeta)
; / .	Subir / Bajar en la lista (mantener para desplazamiento rápido)
N / B	Pista siguiente / anterior
/ / ,	Avanzar / Retroceder (5s o 10s, mantener para avanzar rápido)
[ / ] o - / =	Bajar / Subir volumen (0–21)
A	Amp Boost por hardware (100% / 150% / 200%)
S	Activar / Desactivar Shuffle instantáneo
L	Modo de repetición (NORM / RP1X / RPINF / RPALL)
R	Invertir orden de reproducción (FWD ↔ REV)
F	Explorador de carpetas
T / V / G	Tema / Visualizador / Tamaño de fuente
M / I / Esc	Ajustes / Ayuda de teclas / Volver
G0 (botón lateral)	Modo Bolsillo: 1× Play/Pausa · 2× Siguiente · 3× Anterior
Funciones a medida y apoyo
Este proyecto es 100% gratuito y de código abierto — y siempre lo será.

Por desgracia, las facturas no se pagan solas 💀

Si MP3+ convirtió tu Cardputer en tu reproductor favorito de bolsillo, puedes invitarme un café — o encargarme funciones personalizadas, animaciones de inicio, temas o compilaciones a medida por un precio accesible:

Apoyar el proyecto:

Desarrollo a medida: Farbender@protonmail.com

Créditos y licencia:
Construido con ESP32-audioI2S y M5Unified/M5GFX.

Distribuido bajo licencia GNU General Public License v3.0 (GPL-3.0).
