/* Allocation hooks used by buffers.c; implemented in media.c (CardputerOS). */
#pragma once
void *helix_malloc(int size);
void helix_free(void *ptr);
