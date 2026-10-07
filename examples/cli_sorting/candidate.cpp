#include <iostream>
#include <vector>

int main() {
  int count;
  if (!(std::cin >> count) || count < 0 || count > 100000) return 1;
  std::vector<long long> values(count);
  for (auto& value : values) {
    if (!(std::cin >> value)) return 1;
  }
  // EVOLVE-BLOCK-START
  for (int i = 1; i < count; ++i) {
    const auto value = values[i];
    int j = i;
    while (j > 0 && values[j - 1] > value) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = value;
  }
  // EVOLVE-BLOCK-END
  for (const auto value : values) std::cout << value << ' ';
  std::cout << '\n';
}
