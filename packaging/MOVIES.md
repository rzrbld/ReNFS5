# Movies

The port decodes EA-MAD intro movies in-process with FFmpeg. Game movie files
must come from your own installation; none are included in the port package.
Use the launcher's Movies option to enable or disable playback. A button press
can skip a playing movie.

Decoded frames are presented through SDL in the game window, with audio through
the shared mixer. Playback ends at media EOF after queued audio drains.
