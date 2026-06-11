/* movie_player.h -- in-engine video playback for the intro/credits movies
 *
 * On Android the game plays its .m4v movies through the Java MediaPlayer on
 * a view above the game surface and just polls isMoviePlaying(). Here we
 * decode them with FFmpeg on a worker thread and overlay the frames right
 * before each eglSwapBuffers call.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __MOVIE_PLAYER_H__
#define __MOVIE_PLAYER_H__

// starts playback; returns 1 on success, 0 when the file can't be played
// (the game then proceeds as if the movie ended)
int movie_play(const char *path, int skippable);

// stops playback and frees all resources; safe to call when not playing
void movie_stop(void);

// skips the movie if the game flagged it as skippable; called by the main
// loop on button presses (android handled skip in the Java video view)
void movie_skip(void);

// also finalizes a finished movie; the game polls this every frame
int movie_is_playing(void);

// subtitle line pushed through GamePlatformServices.setMovieText; empty or
// NULL clears the current line. drawn over the video by the GL overlay.
void movie_set_text(const char *text);
void movie_set_text_scale(int scale);

// eglSwapBuffers replacement for the import table: draws the current video
// frame over the game's output while a movie is active
unsigned int eglSwapBuffersHook(void *display, void *surface);

// called once per main loop iteration; presents the movie itself when the
// game has stopped rendering while waiting for it
void movie_main_loop_tick(void);

#endif
