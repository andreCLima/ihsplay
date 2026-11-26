```
    ______  _______       __
   /  _/ / / / ___/____  / /___ ___  __      In-Home
   / // /_/ /\__ \/ __ \/ / __ `/ / / /     Streaming
 _/ // __  /___/ / /_/ / / /_/ / /_/ /     Player
/___/_/ /_//____/ .___/_/\__,_/\__, /     For Steam
               /_/            /____/
```

IHSplay is an SDL2 based Steam Link Client, primarily targeting webOS TV
and Raspberry Pi.

The application will be providing a TV remote / gamepad friendly UI, and
low latency video / audio output, as well as generic mouse / keyboard /
gamepad support.

---

![Home UI](https://user-images.githubusercontent.com/830358/219081008-e5eb646a-76fa-4683-9869-35e3923618ce.png)

![Streaming Overlay](https://user-images.githubusercontent.com/830358/207047480-8cc96496-4fab-4a0e-8d1a-896183c54a6e.png)


Stay tuned!

## Log for bug reports

The app writes a log next to its folder; on webOS:
`/media/developer/apps/usr/palm/applications/org.mariotaku.ihsplayihsplay.log` (the run before is
`org.mariotaku.ihsplayihsplay.1.log`). For a detailed log, create an empty file
`/media/developer/apps/usr/palm/applications/org.mariotaku.ihsplaydebug` (over SSH:
`touch` that path) and restart the app; delete it to go back to the smaller normal log. The
Support > Detailed log screen shows both paths and whether the detailed log is on. While streaming,
the remote's red, green, yellow and blue keys put a mark in the log, to point at the moment a
problem happened.

### Log para relatar problemas (pt-BR)

O app grava um log ao lado da própria pasta; no webOS:
`/media/developer/apps/usr/palm/applications/org.mariotaku.ihsplayihsplay.log` (a execução
anterior fica em `org.mariotaku.ihsplayihsplay.1.log`). Para o log detalhado, crie um arquivo
vazio `/media/developer/apps/usr/palm/applications/org.mariotaku.ihsplaydebug` (por SSH: `touch`
nesse caminho) e reinicie o app; apague-o para voltar ao log normal, que fica menor. A tela
Support > Detailed log mostra os dois caminhos e se o log detalhado está ligado. Durante o streaming,
as teclas vermelha, verde, amarela e azul do controle remoto põem uma marca no log, para apontar o
momento em que o problema aconteceu.
