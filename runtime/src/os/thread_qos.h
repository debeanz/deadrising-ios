#pragma once

#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

// iPhone / iPad: the threads a frame waits for (the game's, the renderer's, the GPU command processor) ask for the
// fast cores. Without a quality-of-service class iOS may run them on the efficiency cores (an iPhone 15 has 2 fast and
// 4 efficiency cores): 2026-11-25, the game's draw hooks took 15-30 ms a frame there against 3-6 ms on a PC.
// Call on the thread itself, as it starts. Elsewhere it does nothing.
inline void SetThreadInteractive()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}
