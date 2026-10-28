// Reproductor: cola de temas, shuffle/repeat y decodificación MP3 en una
// tarea propia (núcleo 0) para que la interfaz no corte el audio.
#pragma once

#include <Arduino.h>
#include <vector>

namespace player {

enum class Repeat : uint8_t { Off, All, One };

void begin(uint8_t volume, bool shuffle, Repeat repeat);
void update(); // llamar en loop(): pasa al siguiente tema cuando termina

void play(const std::vector<uint16_t> &queue, int index);
void stop(); // corta y vacía la cola
void togglePause();
void next();
void prev();
void seek(int deltaSec);

int current(); // tema que suena (o en pausa), -1 si no hay
bool paused();
uint32_t elapsed(); // segundos
int queuePos();     // 0..queueSize()-1, en el orden en que se reproduce
int queueSize();

bool shuffle();
void setShuffle(bool on);
Repeat repeat();
void setRepeat(Repeat r);
uint8_t volume();
void setVolume(uint8_t v);

} // namespace player
