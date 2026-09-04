/* Declarations, types and the shapes that carry fields. */
#include <stdint.h>
#define SQUARE(x) ((x) * (x))

typedef struct Point { int x, y; } Point;
typedef union Value { int i; double d; char *s; } Value;
typedef enum Colour { RED = 1, GREEN, BLUE = 8 } Colour;

static const char *const NAMES[] = {"red", "green", "blue"};
extern int (*handler)(int, void *);
volatile unsigned long counter;

int add(int a, int b);
void each(const char *restrict s, int n[static 4]);
static inline int square(int x) { return SQUARE(x); }

struct Nested { struct { int inner; } anon; int flexible[]; };
