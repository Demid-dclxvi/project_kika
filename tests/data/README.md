# Данные для тестов

| Файл | Что это | Откуда |
| --- | --- | --- |
| `cantilever_flat.gcode` | Балка 100×10×10 мм, напечатана плашмя: все нити вдоль балки, 100 % заполнение, 2 стенки | `prototype/tests/mini_slicer.py`: `slice_mesh(trimesh.creation.box((100, 10, 10)), flavor="orca", infill=1.0, solid_angles=(0,), walls=2, pattern="grid")` |
| `cantilever_upright.gcode` | Та же балка, напечатана стоя (длина по Z) | то же, `box((10, 10, 100))` |

По ним тест `tests/analysis/test_analysis.cpp` сверяет прогиб и запас прочности консольной балки с аналитикой, как `prototype/tests/test_cantilever.py`.

Примеры кронштейнов лежат в `prototype/examples/` и используются оттуда.
