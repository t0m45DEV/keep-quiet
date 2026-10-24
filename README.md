# KEEP QUIET

KEEP QUIET es un juego para dos placas ESP32 con pantalla, botones y micrófono.
Las placas juegan al mismo tiempo e intercambian instrucciones y resultados por
ESP-NOW.

## Inicio

1. Enciende ambas placas con el mismo firmware.
2. Después de la portada y la presentación, cada placa busca a la otra mediante
   ESP-NOW. Ambas deben estar encendidas para completar la conexión.
3. En cada placa, selecciona la dificultad con **UP** y **DOWN**, y confirma
   con **START**.
4. La partida comienza cuando las dos placas confirmaron. La selección de
   dificultad de la placa A se aplica en ambas.

La placa A se determina automáticamente comparando las direcciones MAC de las
dos placas. ESP-NOW funciona en el canal 1.

## Cómo se juega

Cada placa genera una instrucción para que la cumpla la otra placa. Por eso, la
pantalla muestra el código que debe ejecutar el otro jugador; en paralelo, el
jugador local debe ejecutar el código que recibe de su compañero. Las placas
mantienen sus propios temporizadores y leen sus propios botones.

Una instrucción combina una dirección y una selección de colores:

| Código | Dirección |
| --- | --- |
| 1 | UP + LEFT |
| 2 | UP + RIGHT |
| 3 | DOWN + RIGHT |
| 4 | DOWN + LEFT |

Los botones **KEY1**, **KEY2** y **KEY3** seleccionan los colores **R**, **G**
y **B**. Se pueden combinar para formar los códigos **Y**, **M**, **C** y **W**;
si no se requiere color, no pulses ninguno. La selección de color se alterna
cada vez que se pulsa su botón.

Mantén la combinación direccional estable brevemente para confirmar la
respuesta. Si es correcta, esa placa envía el resultado a su compañero y este
genera una nueva instrucción para la siguiente respuesta. Cada placa avanza su
propia secuencia cuando el otro jugador cumple correctamente su código.

## Temporizador, ruido y puntaje

- El temporizador de cada pantalla corresponde a la instrucción que debe
  resolver ese jugador. El límite inicial es de 10 segundos.
- Resolver correctamente suma un punto local y reduce un poco el tiempo
  disponible para instrucciones posteriores.
- La barra **LOUD** muestra el nivel del micrófono. Si alcanza `SILENCE_LEVEL`
  (80 en la configuración actual), la placa detecta demasiado ruido y termina
  la partida. Hay un breve período de gracia al iniciar cada instrucción.
- Si se acaba el tiempo o se supera el nivel de ruido, la placa comunica la
  derrota a la otra.
- En la pantalla final, ambas placas muestran el total de puntos de los dos
  jugadores: el puntaje local más el recibido por ESP-NOW.

## Controles de pantalla

- **UP / DOWN**: elegir dificultad antes de la partida o seleccionar una
  opción en la pantalla de derrota.
- **START**: confirmar la dificultad o la opción seleccionada. Después de la
  animación de derrota, START pasa al mensaje final.
- **Volver a jugar**: inicia una nueva partida con la dificultad anterior;
  ambas placas deben confirmar el inicio.
- **Cambiar modo**: vuelve a la selección de dificultad.
