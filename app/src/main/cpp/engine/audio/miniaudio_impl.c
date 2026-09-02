/* Единственная единица трансляции с реализацией miniaudio (остальные включают только API).
   Урезаем ненужные декодеры/энкодинг — грузим лишь WAV; меньше кода и времени сборки. */
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
