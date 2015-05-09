package danbroid.andrudio;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.AudioTrack.OnPlaybackPositionUpdateListener;
import android.util.Log;

/**
 * A simple audio player that wraps a {@link AudioTrack} instance.
 * This is the second tier API that resides on top of {@link LibAndrudio}.
 * 
 * 
 * @author dan
 */
public class AudioPlayer implements LibAndrudio.AudioStreamListener,
    OnPlaybackPositionUpdateListener {

  private static final String TAG = AudioPlayer.class.getName();

  private long handle = 0;

  private int sampleFormat;

  private int sampleRateInHz;

  private int channelConfig;

  private AudioTrack audioTrack;

  private State state;

  /*  private final Handler handler = new Handler(new Handler.Callback() {
      @Override
      public boolean handleMessage(Message msg) {
        switch (msg.what) {
        case EVENT_SEEK_COMPLETE:
          onSeekComplete();
          return true;
        case EVENT_STATE_CHANGE:
          onStateChange(stateValues[msg.arg1], stateValues[msg.arg2]);
          return true;
        default:
          Log.e(TAG, "unhandled event: " + msg.what);
        }
        return false;
      }
    });*/

  public enum State {
    IDLE, INITIALIZED, PREPARING, PREPARED, STARTED, PAUSED, COMPLETED, STOPPED, ERROR, END;
  }

  private static final State[] stateValues;
  static {
    stateValues = State.values();
  }

  public AudioPlayer() {
    super();
    handle = LibAndrudio.create();
    LibAndrudio.setListener(handle, this);
  }

  protected void onStateChange(State old_state, State state) {
    Log.v(TAG, "onStateChange() " + old_state + " -> " + state);
    this.state = state;
    if (old_state == State.STARTED && state != State.PAUSED
        && state != State.COMPLETED) {
      Log.v(TAG, "audioTrack.stop()");
      audioTrack.stop();
    } else if (state == State.STARTED) {
      Log.v(TAG, "audioTrack.play()");
      audioTrack.play();
    } else if (state == State.PAUSED) {
      Log.v(TAG, "audioTrack.pause()");
      audioTrack.pause();
    } else if (state == State.PREPARED) {
      onPrepared();
    } else if (state == State.COMPLETED) {
      if (audioTrack != null) {
        Log.v(TAG, "audioTrack.stop()");
        audioTrack.stop();
      }
    }
  }

  /**
   * resets, sets the datasource and prepares the player
   * 
   * @param url
   * the datasource
   */

  public void play(String url) {
    Log.i(TAG, "play() :" + url);
    reset();
    setDataSource(url);
    prepareAsync();
  }

  public void prepareAsync() {
    LibAndrudio.prepareAsync(handle);
  }

  public synchronized void reset() {
    LibAndrudio.reset(handle);
    if (audioTrack != null) {
      audioTrack.release();
      audioTrack = null;
    }
  }

  public synchronized void release() {
    Log.i(TAG, "release()");
    if (handle != 0) {
      LibAndrudio.destroy(handle);
      handle = 0;
    }
  }

  @Override
  protected void finalize() throws Throwable {
    if (handle != 0)
      release();
    super.finalize();
  }

  @Override
  public synchronized AudioTrack prepareAudio(int sampleFormat,
      int sampleRateInHZ, int channelConfig) {
    Log.d(TAG, "prepareAudio() format: " + sampleFormat + " rate: "
        + sampleRateInHZ + " channels: " + channelConfig);

    boolean changed = (this.sampleFormat != sampleFormat
        || this.sampleRateInHz != sampleRateInHZ || this.channelConfig != channelConfig);

    this.sampleFormat = sampleFormat;
    this.channelConfig = channelConfig;
    this.sampleRateInHz = sampleRateInHZ;
    if (changed && this.audioTrack != null) {
      audioTrack.release();
      audioTrack = null;
    }
    int chanConfig = (channelConfig == 1) ? AudioFormat.CHANNEL_OUT_MONO
        : AudioFormat.CHANNEL_OUT_STEREO;

    int minBufferSize = AudioTrack.getMinBufferSize(sampleRateInHz, chanConfig,
        AudioFormat.ENCODING_PCM_16BIT) * 4;
    Log.v(TAG, "minBufferSize: " + minBufferSize);

    if (audioTrack == null) {
      audioTrack = new AudioTrack(AudioManager.STREAM_MUSIC, sampleRateInHz,
          chanConfig, AudioFormat.ENCODING_PCM_16BIT, minBufferSize,
          AudioTrack.MODE_STREAM);
      audioTrack.setPlaybackPositionUpdateListener(this);
      audioTrack.setPositionNotificationPeriod(sampleRateInHZ);
    }

    return audioTrack;
  }

  @Override
  public final void handleEvent(int what, int arg1, int arg2) {
    // handler.sendMessage(handler.obtainMessage(what, arg1, arg2));
    switch (what) {
    case EVENT_SEEK_COMPLETE:
      onSeekComplete();
      break;
    case EVENT_STATE_CHANGE:
      onStateChange(stateValues[arg1], stateValues[arg2]);
      break;
    default:
      Log.e(TAG, "handleEvent() " + what + ":" + arg1 + ":" + arg2
          + " not handled");
      break;
    }
  }

  public AudioTrack getAudioTrack() {
    return audioTrack;
  }

  public void seekTo(int msecs) {
    LibAndrudio.seekTo(handle, msecs, false);
  }

  public void start() {
    LibAndrudio.start(handle);
  }

  public void stop() {
    LibAndrudio.stop(handle);
  }

  public void pause() {
    LibAndrudio.togglePause(handle);
  }

  /**
   * Will call {@link #start()}. Do not call super to override this behaviour.
   */
  protected void onPrepared() {
    Log.v(TAG, "onPrepared()");
    onPeriodicNotification(audioTrack);
  }

  @Override
  public void onMarkerReached(AudioTrack track) {
  }

  @Override
  public void onPeriodicNotification(AudioTrack track) {
    if (state == State.PREPARING)
      return;
    Log.v(TAG, "onPeriodicNotification() position:" + getPosition()
        + " head position: " + track.getPlaybackHeadPosition()
        + " playback rate: " + track.getPlaybackRate() + " duration: "
        + getDuration());
  }

  /**
   * 
   * @return playback position in millis or -1 if track is invalid
   */

  public int getPosition() {
    return LibAndrudio.getPosition(handle);
  }

  /**
   *
   * @return length of track in millis
   */
  public int getDuration() {
    return LibAndrudio.getDuration(handle);
  }

  public void printStatus() {
    LibAndrudio.printStatus(handle);
  }

  protected void onSeekComplete() {
    Log.v(TAG, "onSeekComplete()");
  }

  public void setDataSource(String url) {
    LibAndrudio.setDataSource(handle, url);
  }

  public boolean isLooping() {
    return LibAndrudio.isLooping(handle);
  }
}