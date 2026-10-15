# Как мы работаем: код-стайл и Git Flow

## 1. Код-стайл

### C (стандарт C11)

Форматирование автоматическое: настройки лежат в `.clang-format`, общие для редакторов — в `.editorconfig`.

```bash
make format   # отформатировать код
make lint     # проверить без изменений (запускаем перед каждым коммитом)
```

| Правило | Как у нас |
|---|---|
| Отступы | 4 пробела, без табов (кроме Makefile) |
| Длина строки | до 100 символов |
| Скобки | `{` у функции на новой строке, у `if/for/while` на той же (стиль Linux) |
| Имена | `snake_case` для функций, переменных и типов; `UPPER_CASE` для макросов и констант |
| Префикс модуля | публичные функции начинаются с имени модуля: `db_connect`, `ui_print_search`, `import_file`, `parse_line` |
| Внутренние функции | `static`, без префикса: `copy_flush`, `parse_bsd_ts` |
| Заголовки | include guard вида `LOGPARSER_<MODULE>_H`; в `.h` только то, что нужно другим модулям |
| Указатели | звёздочка у имени: `char *p` |
| Ошибки | функции возвращают `0` при успехе и `-1` при ошибке (или `NULL`); сообщения выводятся в `stderr` |
| Память | кто выделил память, тот её и освобождает; у каждого `*_begin` есть парный `*_end` |
| Комментарии | на русском, объясняют *зачем*, а не *что* |
| Предупреждения | собираем с `-Wall -Wextra -Wpedantic`; код с warning-ами не мержим |

### SQL

- Ключевые слова пишем ЗАГЛАВНЫМИ, имена в `snake_case`, таблицы во множественном числе (`log_entries`, `sources`).
- Схема идемпотентна (`IF NOT EXISTS`, `CREATE OR REPLACE`), поэтому `init` можно запускать повторно.
- Значения от пользователя передаются **только через параметры** `$1, $2…` (`PQexecParams`). Склеивать SQL со строками ввода запрещено: так мы защищены от SQL-инъекций.

### Сообщения коммитов: Conventional Commits

```
<тип>(<модуль>): <что сделано, в повелительном наклонении>
```

Типы: `feat` (новая функция), `fix` (исправление), `refactor`, `test`, `docs`, `chore` (сборка и настройки).
Модули: `parser`, `importer`, `db`, `schema`, `ui`, `cli`.

Примеры:
```
feat(parser): поддержка ISO 8601 и journalctl -o short-iso
fix(db): экранирование табуляции в COPY
docs(schema): ER-диаграмма базы
```

## 2. Git Flow

### Ветки

| Ветка | Назначение | Кто пишет |
|---|---|---|
| `main` | только стабильные релизы, каждый с тегом `vX.Y.Z` | никто напрямую, только merge из `release/*` и `hotfix/*` |
| `develop` | интеграционная ветка: сюда сливаются готовые фичи | никто напрямую, только через Pull Request |
| `feature/<кратко>` | одна задача, ответвляется от `develop` | автор задачи |
| `release/<версия>` | подготовка релиза: правим версию и мелкие баги, новых фич не добавляем | тимлид |
| `hotfix/<кратко>` | срочное исправление в релизе, ответвляется от `main` | кто чинит |

```
main     ●────────────────────────────●──────────●   v0.1.0, v0.1.1
          \                          / \        /
release    \                ●──●──●─┘   \      /
            \              /             \    /
hotfix       \            /               ●──●
              \          /                    \
develop        ●──●──●──●──●──●────────────────●
                \    /  \     /
feature          ●──●    ●──●    (parser, db-schema, cli, importer)
```

### Правила

1. На каждую задачу заводим отдельную ветку от свежего `develop`.
2. Коммитим маленькими осмысленными шагами, сообщения пишем по Conventional Commits.
3. Задача готова: пушим ветку и открываем Pull Request в `develop`.
4. Нужно минимум **одно ревью** от другого участника, `make` и `make test` должны проходить.
5. Мержим через **merge commit** (`--no-ff`), чтобы в истории было видно, какие коммиты относятся к какой фиче. После мержа ветку удаляем.
6. `main` и `develop` защищены в настройках GitHub: прямой push запрещён.

### Распределение задач

| Участник | Роль | Ветки |
|---|---|---|
| Александр | бэкенд | `feature/parser`, `feature/importer` |
| loha | база данных | `feature/db-schema` |
| Руслан | фронтенд (CLI) | `feature/cli` |

### Шпаргалка команд

```bash
# начать задачу
git switch develop && git pull
git switch -c feature/db-schema

# работать
git add sql/schema.sql
git commit -m "feat(schema): таблицы log_entries, sources, parse_errors"
git push -u origin feature/db-schema
# → на GitHub: Compare & pull request → base: develop → назначить ревьюера

# после мержа PR
git switch develop && git pull
git branch -d feature/db-schema

# релиз (тимлид)
git switch -c release/0.1.0 develop
#   поднять VERSION в src/main.c, коммит
git switch main && git merge --no-ff release/0.1.0 && git tag -a v0.1.0 -m "v0.1.0"
git switch develop && git merge --no-ff release/0.1.0
git push origin main develop --tags

# срочный фикс
git switch -c hotfix/copy-escape main
#   исправить, коммит, затем merge в main (тег v0.1.1) и в develop
```
