# Movies

The game plays its EA-MAD movies with its own player: the intro movies, the
main menu's background video and the Porsche Chronicles videos, which show
inside their screen with the game's own controls. Game movie files must come
from your own installation; none are included in the port package. Use the
launcher's Movies option to enable or disable playback.

`NFS5_GAME_MOVIES=0` switches to the port's built-in FFmpeg player instead,
which plays each movie fullscreen (a button press skips it). It blocks the game
while a movie plays, so Porsche Chronicles videos do not work properly with it;
it is kept only as a fallback.
