// oax_stablesort.h: a stable merge sort with qsort's signature, used in
// place of qsort by RecastContour.cpp and DetourNavMeshBuilder.cpp (oax
// patch, see README.oax.md). qsort is not stable and C libraries order equal
// keys differently (glibc vs the wasm build's musl), which made the native
// and wasm navmeshes differ; a stable sort orders equal keys by input order
// on every build.
#ifndef OAX_STABLESORT_H
#define OAX_STABLESORT_H

#include <stdlib.h>
#include <string.h>

static inline void oaxStableSortRun(unsigned char* a, unsigned char* tmp, size_t n, size_t size,
	int (*cmp)(const void*, const void*))
{
	if (n < 2)
		return;
	size_t h = n / 2;
	oaxStableSortRun(a, tmp, h, size, cmp);
	oaxStableSortRun(a + h * size, tmp, n - h, size, cmp);
	size_t i = 0, j = h, k = 0;
	while (i < h && j < n)
	{
		// take from the right only when strictly smaller: stable
		if (cmp(a + j * size, a + i * size) < 0)
			memcpy(tmp + (k++) * size, a + (j++) * size, size);
		else
			memcpy(tmp + (k++) * size, a + (i++) * size, size);
	}
	while (i < h)
		memcpy(tmp + (k++) * size, a + (i++) * size, size);
	while (j < n)
		memcpy(tmp + (k++) * size, a + (j++) * size, size);
	memcpy(a, tmp, n * size);
}

static inline void oaxStableSort(void* base, size_t n, size_t size, int (*cmp)(const void*, const void*))
{
	if (n < 2)
		return;
	unsigned char* tmp = (unsigned char*)malloc(n * size);
	if (!tmp)
		return;
	oaxStableSortRun((unsigned char*)base, tmp, n, size, cmp);
	free(tmp);
}

#endif
