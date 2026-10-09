#pragma once
// Простой пул потоков для параллельных циклов ядра (без сторонних библиотек).

#include <algorithm>
#include <cstddef>
#include <functional>

#include "kika/parallel.hpp"

namespace kika::util {

// Выполняет fn(k) для k = 0 … n_tasks−1 на всех потоках пула и ждёт завершения.
// Исключение из задачи пробрасывается вызывающему. Вложенные вызовы выполняются последовательно.
void run_tasks(std::size_t n_tasks, const std::function<void(std::size_t)>& fn);

// Параллельный цикл по [begin, end): fn(lo, hi) для кусков не меньше grain.
// Маленькие диапазоны выполняются сразу в вызывающем потоке.
template <class F>
void parallel_for(std::size_t begin, std::size_t end, std::size_t grain, F&& fn) {
  if (end <= begin) return;
  const std::size_t n = end - begin;
  const auto threads = static_cast<std::size_t>(thread_count());
  if (threads <= 1 || n <= grain) {
    fn(begin, end);
    return;
  }
  // по нескольку кусков на поток — для выравнивания нагрузки
  std::size_t chunks = std::min(threads * 4, (n + grain - 1) / grain);
  const std::size_t step = (n + chunks - 1) / chunks;
  chunks = (n + step - 1) / step;
  run_tasks(chunks, [&](std::size_t k) {
    const std::size_t lo = begin + k * step;
    fn(lo, std::min(end, lo + step));
  });
}

}  // namespace kika::util
