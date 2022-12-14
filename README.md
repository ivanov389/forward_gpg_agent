# forward_gpg_agent

[![License](https://img.shields.io/badge/license-BSD-blue.svg)](https://github.com/ivanov389/forward_gpg_agent/blob/main/LICENSE.txt)

**forward_gpg_agent** - костыль с консольным интерфейсом для проброса GPG агента в интернет сокет и PuTTY агента в интернет сокет или именованный канал. Для `Windows Operating System™`. В Линуксе такие костыли не нужны, но если очень хочется, можно использовать socat.

# FAQ

* [Зачем это нужно?](#user-content-зачем-это-нужно)
* [Cool story bro, но вопрос был: зачем это нужно, если есть win-gpg-agent](#user-content-cool-story-bro-но-вопрос-был-зачем-это-нужно-если-есть-win-gpg-agent)?
* [Уговорил. Где взять софтину?](#user-content-уговорил-где-взять-софтину)
* [Как пользоваться?](#user-content-как-пользоваться)

## Зачем это нужно?

[GnuPG](https://www.gnupg.org/) агент использует пять Unix-сокетов для взаимодействия с внешним миром.
* `gpg-agent.socket` - для взаимодействия с "головной" `gpg`. Через него можно делать все, что хочется, например, получать/заменять/удалять приватные ключи. Лучше этот сокет никуда не пробрасывать😀.
* `gpg-agent-extra.socket` -  ограниченный в правах `gpg-agent.socket`. Специально создан для проброса на удаленную машину. Позволяет использовать `gpg` на удаленной машине, но приватных ключей не выдает и на локальной машине ничего не меняет.
* `gpg-agent-ssh.socket` - для взаимодействия с OpenSSH. Через него реализуется OpenSSH протокол обмена ключами. Таким образом, `gpg-agent` может использоваться `ssh` и `ssh-add` для доступа к ключам аутентификации вместо "родного" `ssh-agent`-а.
* `gpg-agent-browser.socket` - для взаимодействия с браузерами.
* `dirmngr.socket` - для взаимодействия с OpenPGP серверами ключей.

[Gpg4win](https://www.gpg4win.org/) - вариант GnuPG для Windows. Как и любой порт для Windows, реализован ~~<span style="color:white">через жопу</span>~~ не вполне идентично оригиналу. Поддержка Unix-сокетов в Windows [появилась](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/) начиная с Windows 10 сборки 17063 (тоже "не вполне идентично оригиналу", но для наших целей было бы достаточно). Gpg4win существовала и до указанной версии Windows, поэтому все `gpg-agent.socket`-ы на самом деле - обычные файлы в файловой системе, содержащие номер локального порта, который слушает `gpg-agent`, и несколько рандомных байт - что-то вроде токена для авторизации. Видимо, в целях обратной совместимости в "gpg сокетах" ничего не изменилось и с появлением поддержки в Windows Unix-сокетов. Значит, если требуется воспользоваться хранилищем gpg ключей с локальной Windows машины на удаленной машине, нужен костыль, который пробросит "gpg сокет" в локальный интернет порт. Далее уже хорошо защищенными сторонними средствами (ssh туннель, например) можно пробросить этот локальный порт на удаленную машину. `forward_gpg_agent` этот костыль и есть.

История развивалась так. Первая версия `forward_gpg_agent`-а написана, велосипед собран: gpg используется на удаленном Linux, хранилище ключей - на локальном Windows. Подпись и шифрование/дешифрование работают, а вот удаленный ssh ключей аутентификации не видит. Значит, `gpg-agent-extra.socket` пробросился, а `gpg-agent-ssh.socket` застрял по дороге. Это хорошо: полдела сделано. В логах `gpg-agent`-а - полная идиллия: агент открыл порт для нового соединения с OpenSSH, принял "токен авторизации" и сразу же закрыл соединение без всяких ошибок еще до первого запроса. Все стало кристально ясно: без пол-литры Обуховской не разобраться. Вентилирование интернета выдало [тикет](https://dev.gnupg.org/T3883) от апреля 2018 года, в обсуждении которого прямым текстом написано: в коде GnuPG под Windows для `gpg-agent-ssh.socket`-а стоит затычка, он не работает, потому что "ну кому нужен ssh под Винду". Вторая, расширенная, версия `forward_gpg_agent`-а была написана в конце лета 2022 года, когда Автор (`forward_gpg_agent`-а, а не тикета) подумал, что 4 года от открытия тикета - достаточный срок, чтобы не закрывать его никогда, и решил накостылять чего-нибудь сам. Плохо подумал: ждать оставалось пару месяцев. Из того же тикета понятно, что с 2022.10.14, начиная с Gpg4win v4.0.5 (GnuPG v2.3.9), `gpg-agent` под Windows может открывать именованный канал для локального общения с [Win32-OpenSSH](https://github.com/powershell/Win32-OpenSSH). И совсем хорошо, что Win32-OpenSSH умеет пробрасывать именованные каналы через туннель. Таким образом, после 2022.10.14 проблема проброса `gpg-agent-ssh.socket`-а с локальной Windows на удаленную машину решена. Однако, из-за того, что терпение не относится к положительным качествам Автора, `forward_gpg_agent` имеет аналогичную функциональность. Gpg4win была создана до того, как в Microsoft в сентябре 2015 решили, что `Windows Operating System™` пользуется уже больше пары человек, и потому ей бы не помешала своя реализация OpenSSH. В те времена самым популярным ssh клиентом для Windows был PuTTY (версии которого есть для Windows 95, Карл!), поэтому `gpg-agent` умеет маскироваться под PAgeant (PuTTY ssh authentication agent). Вот почему вместо проброса нерабочего `gpg-agent-ssh.socket`-а в `forward_gpg_agent`-е реализован проброс PAgeant-а в интернет сокет или именованный канал. Последний нужен был для общения с Win32-OpenSSH, так как его родной `ssh-agent` перманентно [хранит](https://github.com/PowerShell/Win32-OpenSSH/issues/1487) приватные ключи в реестре (на диске, да), защищенные только паролем локального пользователя, и удаляет только по явной команде `ssh-add -D`. Ну а кто не ставит минимум 15-ти значные пароли на вход в Винду - те сами виноваты, не так ли?

## Cool story bro, но вопрос был: зачем это нужно, если есть [win-gpg-agent](https://github.com/rupor-github/win-gpg-agent)?

Вообще, кроме Автора, это никому и не нужно. Да, есть много самопала с теми же или почти теми же возможностями: [win-gpg-agent](https://github.com/rupor-github/win-gpg-agent), [wsl-ssh-pageant](https://github.com/benpye/wsl-ssh-pageant), [wsl-gpg-bridge](https://github.com/Riebart/wsl-gpg-bridge), [ssh-agent-adapter](https://github.com/manojampalam/ssh-agent-adapter). Однако, в вопросах доступа к приватным ключам Автор не доверяет чужому самопалу - только своему, это первое. Второе: среди этих четырех репозиториев нет ни одного с кодом на C/C++. Первые два - Go, третий - Python, четвертый - C, но он не работает. Теперь справедливость восстановлена: есть один рабочий на C++👍. Третье: Автор давно хотел попробовать чего-нибудь накодить с Беркли сокетами, а тут такой случай подвернулся. И, конечно, четвертое: github-профиль сам себя не наполнит.

## Уговорил. Где взять софтину?

### Качнуть

[~~первый и~~ последний релиз](https://github.com/ivanov389/forward_gpg_agent/releases). И постараться избавиться от засевшего в голове назойливого вопроса: а, случаем, не может такого быть, что прога сливает приватные ключи налево?

### Компильнуть.

Прежде всего, прочитать 3000 строк лапши из кода и убедиться, что прога не сливает приватные ключи налево ~~, а только направо~~. Поставить [git](https://git-scm.com/download/win), поставить [cmake](https://cmake.org/download/). Дальше - просто.
```bat
git.exe clone https://github.com/ivanov389/forward_gpg_agent.git
cd forward_gpg_agent
mkdir build
cd build
```
Если стоит Visual Studio 2019
```bat
cmake.exe -G "Visual Studio 16 2019" -T host=x86 -A x64 ..
cmake.exe --build . --config Release
```
Если стоит [clang](https://github.com/llvm/llvm-project/releases) и [ninja](https://github.com/ninja-build/ninja/releases)
```bat
cmake.exe -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_C_COMPILER:FILEPATH=clang.exe -DCMAKE_CXX_COMPILER:FILEPATH=clang++.exe -G Ninja ..
cmake.exe --build . --config Release
```

## Как пользоваться?

Для начала, посмотреть встроенную, если можно так назвать, справку: `forward_gpg_agent -?`. Когда все станет понятно, есть несколько вариантов использования.

### Проброс "gpg сокета" в локальный порт

Работает только с `gpg-agent.socket` и `gpg-agent-extra.socket`.

Получаем путь к `gpg-agent-extra.socket`:
```bat
for /f %%i in ('gpgconf --list-dirs agent-extra-socket') do set GPGSocketExtra=%%i
```
Пробрасываем `gpg-agent-extra.socket` в локальный порт 4815:
```bat
forward_gpg_agent.exe -a"%GPGSocketExtra%" -p:4815
```
Далее можно пробросить этот порт в обратный туннель на удаленную Linux машину:
```bat
ssh.exe -R 4815:localhost:4815 RemoteHost
```
Теперь, если на RemoteHost `gpg-agent` убит
```sh
gpgconf --kill gpg-agent
```
можно создать на RemoteHost фэйковый `gpg-agent.socket` и пробросить его в порт 4815:
```sh
socat UNIX-LISTEN:`gpgconf --list-dirs agent-socket`,unlink-close,unlink-early TCP4:localhost:4815 &
```
После этого на RemoteHost будет работать, например, шифрование/дешифрование (`gpg -e`, `gpg -d`) с приватными ключами на локальном `gpg-agent`-е. Естественно, `pinentry` будет запускаться там же, где и `gpg-agent` - на локальной машине.

### Проброс PuTTY агента в локальный порт

Работает с "настоящим" PAgeant-ом из состава PuTTY и с встроенным в `gpg-agent`.

Пробрасываем PAgeant в локальный порт 1623:
```bat
forward_gpg_agent.exe -s:1623
```
Далее можно пробросить этот порт в unix-сокет на удаленной Linux машине через обратный туннель:
```bat
ssh.exe -R /tmp/sshauthsock:localhost:1623 RemoteHost
```
и на RemoteHost указать `ssh` использовать `/tmp/sshauthsock` для аутентификации:
```sh
export SSH_AUTH_SOCK=/tmp/sshauthsock
```
После этого на RemoteHost будет работать ssh-аутентификация с приватными ключами на локальном `gpg-agent`-е (или в PuTTY PAgeant-е). `ssh-add` на RemoteHost сможет добавлять/удалять ssh-ключи в локальный `gpg-agent` (или в PuTTY PAgeant).

### Проброс PuTTY агента в именованный канал

Работает с "настоящим" PAgeant-ом из состава PuTTY и с встроенным в `gpg-agent`. Опция нужна, чтобы Win32-OpenSSH мог работать с ключами аутентификации в PAgeant-е.

Пробрасываем PAgeant в именованный канал:
```bat
forward_gpg_agent.exe -s"pageant42namedpipe"
```
Говорим `ssh` использовать `pageant42namedpipe` для аутентификации:
```bat
set SSH_AUTH_SOCK=\\.\pipe\pageant42namedpipe
```
После этого Win32-OpenSSH будет работать с ключами аутентификации в `gpg-agent`-е (или в PuTTY PAgeant-е).

### Дополнительные опции

`-t:` - количество рабочих потоков, которые будут обрабатывать соединения и передачу данных. Естественно, чтобы нагрузить даже один поток, нужно специально писать dos-сервер. Реализация опции нужна была только чтоб потренироваться в асинхронной многопоточной работе с сокетами. Ну и для понтов 😀.

`-v` - вывод дополнительной информации, а не только сообщений об ошибках. Нужна больше для дебага.

По нажатию CTRL+C `forward_gpg_agent` закрывает открытые порты и именованные каналы и завершает работу.

Естественно, опции `-a`, `-p:`, `-s:` и `-s` можно использовать одновременно.
