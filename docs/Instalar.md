# Configuración para estudiantes

Estos pasos instalan todo lo necesario para compilar y subir los instrumentos del taller.

## 1. Cuentas y herramientas

Instala:

- Cuenta de GitHub: https://github.com/
- Git: https://git-scm.com/downloads
- Arduino IDE 2: https://www.arduino.cc/en/software

Opcional, pero útil:

- GitHub Desktop: https://desktop.github.com/
- Visual Studio Code: https://code.visualstudio.com/

## 2. Agregar soporte para placas RP2040

En Arduino IDE:

1. Abre `File > Preferences`.
2. Busca `Additional Boards Manager URLs`.
3. Agrega esta URL:

```text
https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
```

4. Abre `Tools > Board > Boards Manager`.
5. Busca `pico`.
6. Instala `Raspberry Pi Pico/RP2040/RP2350` de Earle F. Philhower.

## 3. Seleccionar la placa

En Arduino IDE:

1. Abre `Tools > Board`.
2. Selecciona `Raspberry Pi Pico/RP2040/RP2350 > Waveshare RP2040 Zero`.
3. Configura `Tools > USB Stack` como `Adafruit TinyUSB`.
4. Deja `Upload Method` como `Default (UF2)`.

Las configuraciones importantes de la placa son:

```text
Board: Waveshare RP2040 Zero
Core: Raspberry Pi Pico/RP2040/RP2350
USB Stack: Adafruit TinyUSB
Upload Method: Default (UF2)
```

## 4. Instalar librerías

Abre `Sketch > Include Library > Manage Libraries...` e instala:

- `Adafruit TinyUSB Library`
- `MIDI Library`

Para `Dronaldo` y `DrumLoop`, este repositorio incluye:

- `BRAIDS`
- `STMLIB`

Copia estas dos carpetas desde el repositorio:

```text
libraries/BRAIDS/
libraries/STMLIB/
```

a tu carpeta de librerías de Arduino.

Ubicación típica en Windows:

```text
Documents/Arduino/libraries/
```

Las carpetas deberían quedar así:

```text
Documents/Arduino/libraries/BRAIDS/
Documents/Arduino/libraries/STMLIB/
```

Reinicia Arduino IDE después de copiar las carpetas.

## 5. Obtener este repositorio

Opción A: Descargar ZIP

1. En GitHub, haz clic en `Code`.
2. Haz clic en `Download ZIP`.
3. Descomprime el archivo.
4. Abre un archivo `.ino` desde una de las carpetas del taller, por ejemplo `DrumLoop/DrumLoop.ino` o `Dronaldo/Dronaldo.ino`.

Opción B: Usar Git

```bash
git clone https://github.com/mumupedals/TallerRP2040.git
cd TallerRP2040
```

## 6. Subir un sketch

1. Conecta el instrumento por USB.
2. Abre uno de los sketches del taller, como `DrumLoop/DrumLoop.ino` o `Dronaldo/Dronaldo.ino`.
3. Haz clic en `Verify`.
4. Haz clic en `Upload`.

Si la subida falla, mantén presionado el botón `BOOTSEL` del RP2040 Zero mientras conectas el USB, y luego intenta subir el sketch de nuevo. La placa debería aparecer como una unidad USB o como destino de subida.

## 7. Compilar desde la línea de comandos, opcional

Si usas `arduino-cli`, compila con:

```bash
arduino-cli compile --fqbn rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb DrumLoop
arduino-cli compile --fqbn rp2040:rp2040:waveshare_rp2040_zero:usbstack=tinyusb Dronaldo
```
