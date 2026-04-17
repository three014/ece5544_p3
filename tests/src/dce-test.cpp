int foo(int a) {
  int p = a + 231;
start:
  for (int g = 0; g < a; g += 2) {
    int b = 0;
    for (int j = a; j >= 0; j--) {
      if (j == 2) continue;
      if (j + g == a) goto start;
      b = j + g;
    }
    p += b;
  }
  return p;
}
