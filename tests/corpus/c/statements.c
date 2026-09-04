int control(int n) {
  int total = 0;
  for (int i = 0; i < n; i++) {
    if (i % 2 == 0) { continue; }
    else if (i > 100) { break; }
    total += i;
  }
  while (total > 10) { total /= 2; }
  do { total--; } while (total > 0);
  switch (n) {
    case 0: return 0;
    case 1:
    case 2: total = 1; break;
    default: goto done;
  }
done:
  return total;
}

int expressions(int a, int b, int *p) {
  int c = a ? b : -a;
  c += (int)(a * 1.5) - sizeof(int);
  c = a << 2 | b >> 1 & 0xff ^ ~a;
  c = (a > b) && (a != 0) || !b;
  *p = c, p[1] = c;
  return c;
}
