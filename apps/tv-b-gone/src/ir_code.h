// Formato de un código de la base del TV-B-Gone.
#pragma once
#include <Arduino.h>

// El original guarda el valor del timer del ATtiny; acá va la frecuencia en Hz.
#define freq_to_timerval(x) (x)

struct IrCode {
    uint32_t freq;            // portadora en Hz, 0 = sin portadora (LED fijo)
    uint8_t numpairs;         // cantidad de pares encendido/apagado
    uint8_t bitcompression;   // bits por índice en la tabla de tiempos
    const uint16_t *times;    // pares {encendido, apagado} en unidades de 10 µs
    const uint8_t *codes;     // índices empaquetados de a bitcompression bits
};
