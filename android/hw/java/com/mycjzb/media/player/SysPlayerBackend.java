package com.mycjzb.media.player;

import android.content.Context;
import android.media.AudioManager;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.view.Surface;
import android.view.SurfaceHolder;

import java.io.FileDescriptor;
import java.io.IOException;
import java.util.Map;

/**
 * 系统 {@link android.media.MediaPlayer} 薄封装，供 {@code sys:} / 默认 SYSTEM 使用。
 * 监听回调转到本库 MediaPlayer 的 listener（主线程）。
 */
final class SysPlayerBackend {
    interface Host {
        void onPrepared();
        void onCompletion();
        boolean onError(int what, int extra);
        boolean onInfo(int what, int extra);
        void onSeekComplete();
        void onBufferingUpdate(int percent);
        void onVideoSizeChanged(int w, int h);
    }

    private final android.media.MediaPlayer mp;
    private final Handler main;
    private Host host;
    private volatile boolean prepared;
    private volatile boolean released;

    SysPlayerBackend() {
        mp = new android.media.MediaPlayer();
        mp.setAudioStreamType(AudioManager.STREAM_MUSIC);
        main = new Handler(Looper.getMainLooper());
        attach();
    }

    void setHost(Host host) {
        this.host = host;
    }

    void setDataSource(String path) throws IOException {
        prepared = false;
        mp.setDataSource(path);
    }

    void setDataSource(Context ctx, Uri uri, Map<String, String> headers) throws IOException {
        prepared = false;
        if (headers != null && !headers.isEmpty() && ctx != null) {
            mp.setDataSource(ctx, uri, headers);
        } else if (ctx != null) {
            mp.setDataSource(ctx, uri);
        } else {
            mp.setDataSource(uri.toString());
        }
    }

    void setDataSource(FileDescriptor fd, long offset, long length) throws IOException {
        prepared = false;
        mp.setDataSource(fd, offset, length);
    }

    void setSurface(Surface s) {
        if (!released) mp.setSurface(s);
    }

    void setDisplay(SurfaceHolder h) {
        if (!released) mp.setDisplay(h);
    }

    void prepare() throws IOException {
        mp.prepare();
        prepared = true;
    }

    void prepareAsync() {
        mp.prepareAsync();
    }

    void start() {
        mp.start();
    }

    void pause() {
        mp.pause();
    }

    void stop() {
        prepared = false;
        mp.stop();
    }

    void reset() {
        prepared = false;
        mp.reset();
    }

    void release() {
        released = true;
        prepared = false;
        try {
            mp.release();
        } catch (Throwable ignored) {
        }
    }

    void seekTo(int msec) {
        mp.seekTo(msec);
    }

    int getCurrentPosition() {
        if (!prepared) return 0;
        try {
            return mp.getCurrentPosition();
        } catch (Throwable t) {
            return 0;
        }
    }

    int getDuration() {
        if (!prepared) return 0;
        try {
            return mp.getDuration();
        } catch (Throwable t) {
            return 0;
        }
    }

    boolean isPlaying() {
        try {
            return mp.isPlaying();
        } catch (Throwable t) {
            return false;
        }
    }

    int getVideoWidth() {
        try {
            return mp.getVideoWidth();
        } catch (Throwable t) {
            return 0;
        }
    }

    int getVideoHeight() {
        try {
            return mp.getVideoHeight();
        } catch (Throwable t) {
            return 0;
        }
    }

    void setVolume(float l, float r) {
        try {
            mp.setVolume(l, r);
        } catch (Throwable ignored) {
        }
    }

    void setLooping(boolean loop) {
        try {
            mp.setLooping(loop);
        } catch (Throwable ignored) {
        }
    }

    boolean isLooping() {
        try {
            return mp.isLooping();
        } catch (Throwable t) {
            return false;
        }
    }

    boolean isPrepared() {
        return prepared;
    }

    private void attach() {
        mp.setOnPreparedListener(new android.media.MediaPlayer.OnPreparedListener() {
            @Override
            public void onPrepared(android.media.MediaPlayer m) {
                prepared = true;
                post(new Runnable() {
                    public void run() {
                        if (host != null) host.onPrepared();
                    }
                });
            }
        });
        mp.setOnCompletionListener(new android.media.MediaPlayer.OnCompletionListener() {
            @Override
            public void onCompletion(android.media.MediaPlayer m) {
                post(new Runnable() {
                    public void run() {
                        if (host != null) host.onCompletion();
                    }
                });
            }
        });
        mp.setOnErrorListener(new android.media.MediaPlayer.OnErrorListener() {
            @Override
            public boolean onError(android.media.MediaPlayer m, final int what, final int extra) {
                post(new Runnable() {
                    public void run() {
                        if (host != null) host.onError(what, extra);
                    }
                });
                return true;
            }
        });
        mp.setOnInfoListener(new android.media.MediaPlayer.OnInfoListener() {
            @Override
            public boolean onInfo(android.media.MediaPlayer m, final int what, final int extra) {
                post(new Runnable() {
                    public void run() {
                        if (host != null) host.onInfo(what, extra);
                    }
                });
                return false;
            }
        });
        mp.setOnSeekCompleteListener(new android.media.MediaPlayer.OnSeekCompleteListener() {
            @Override
            public void onSeekComplete(android.media.MediaPlayer m) {
                post(new Runnable() {
                    public void run() {
                        if (host != null) host.onSeekComplete();
                    }
                });
            }
        });
        mp.setOnBufferingUpdateListener(new android.media.MediaPlayer.OnBufferingUpdateListener() {
            @Override
            public void onBufferingUpdate(android.media.MediaPlayer m, final int percent) {
                post(new Runnable() {
                    public void run() {
                        if (host != null) host.onBufferingUpdate(percent);
                    }
                });
            }
        });
        mp.setOnVideoSizeChangedListener(new android.media.MediaPlayer.OnVideoSizeChangedListener() {
            @Override
            public void onVideoSizeChanged(android.media.MediaPlayer m, final int w, final int h) {
                post(new Runnable() {
                    public void run() {
                        if (host != null) host.onVideoSizeChanged(w, h);
                    }
                });
            }
        });
    }

    private void post(Runnable r) {
        if (Looper.myLooper() == main.getLooper()) {
            r.run();
        } else {
            main.post(r);
        }
    }
}
