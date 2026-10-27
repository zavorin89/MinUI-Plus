#ifndef BGM_H
#define BGM_H

// background music for the launcher's menus: songs from BGM_PATH (mp3, ogg, flac, wav) in a shuffled order,
// decoded a little ahead on a low priority thread and mixed under the menu sounds

#define BGM_VOLUME_MAX 10

int BGM_isEnabled(void);
void BGM_setEnabled(int enabled); // saved, and starts or stops it
int BGM_getVolume(void); // 1 to BGM_VOLUME_MAX
void BGM_setVolume(int volume); // saved, heard right away
int BGM_countSongs(void); // in BGM_PATH, looked at once

void BGM_start(void); // when it's on and there are songs, fades in
void BGM_pause(void); // fades out without waiting, BGM_start picks up where it left off (the pause menu)
void BGM_quit(void); // fades out quickly and stops, before the audio device closes

#endif
