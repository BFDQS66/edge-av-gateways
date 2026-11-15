#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMetaObject>
#include <QPixmap>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <atomic>
#include <functional>
#include <iostream>
#include <utility>

namespace {

constexpr auto kDefaultUrl = "rtsp://192.168.1.101:18555/approved/obs";

class StreamTile final : public QWidget {
 public:
  explicit StreamTile(const QString& initial_url, QWidget* parent = nullptr)
      : QWidget(parent), video_(new QLabel(this)),
        url_(new QLineEdit(initial_url, this)), status_(new QLabel(this)),
        retry_(new QTimer(this)), watch_(new QTimer(this)) {
    auto* layout = new QVBoxLayout(this);
    auto* controls = new QHBoxLayout;
    auto* connect_button = new QPushButton(QStringLiteral("连接"), this);
    auto* stop_button = new QPushButton(QStringLiteral("停止"), this);
    auto* mute = new QCheckBox(QStringLiteral("静音"), this);
    auto* volume = new QSlider(Qt::Horizontal, this);
    volume->setRange(0, 100);
    volume->setValue(45);
    volume->setFixedWidth(100);
    url_->setPlaceholderText(QStringLiteral("RTSP 地址"));
    controls->addWidget(new QLabel(QStringLiteral("地址:"), this));
    controls->addWidget(url_, 1);
    controls->addWidget(connect_button);
    controls->addWidget(stop_button);
    controls->addWidget(mute);
    controls->addWidget(volume);
    video_->setMinimumSize(640, 360);
    video_->setAlignment(Qt::AlignCenter);
    video_->setStyleSheet(QStringLiteral("background: #111; color: #ddd;"));
    video_->setText(QStringLiteral("等待视频流"));
    layout->addLayout(controls);
    layout->addWidget(video_, 1);
    layout->addWidget(status_);
    layout->addWidget(new QLabel(
        QStringLiteral("传输测试阶段：当前流尚未接入 AI 内容审核"), this));

    retry_->setSingleShot(true);
    retry_->setInterval(3000);
    watch_->setInterval(250);
    frame_clock_.start();
    set_status(QStringLiteral("等待连接"));
    connect(connect_button, &QPushButton::clicked, this, [this] { start(); });
    connect(stop_button, &QPushButton::clicked, this, [this] { stop(); });
    connect(mute, &QCheckBox::toggled, this, [this](bool enabled) {
      muted_ = enabled;
      update_volume();
    });
    connect(volume, &QSlider::valueChanged, this, [this](int value) {
      volume_percent_ = value;
      update_volume();
    });
    connect(retry_, &QTimer::timeout, this, [this] { if (wanted_) open_stream(); });
    connect(watch_, &QTimer::timeout, this, [this] {
      poll_bus();
      if (wanted_ && frame_clock_.elapsed() > 20000) {
        set_status(QStringLiteral("20 秒无画面，3 秒后重连"));
        schedule_retry();
      }
    });
  }

  ~StreamTile() override { stop_pipeline(); }

  void start() {
    wanted_ = true;
    retry_->stop();
    open_stream();
  }

  void stop() {
    wanted_ = false;
    retry_->stop();
    stop_pipeline();
    video_->clear();
    video_->setText(QStringLiteral("已停止"));
    set_status(QStringLiteral("已停止"));
  }

  void on_frame(std::function<void()> callback) { frame_callback_ = std::move(callback); }

 private:
  struct CallbackContext {
    StreamTile* tile;
    uint64_t generation;
  };

  static GstFlowReturn new_sample(GstAppSink* sink, gpointer data) {
    auto* context = static_cast<CallbackContext*>(data);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    GstVideoInfo info;
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) && buffer &&
        gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      const int width = static_cast<int>(GST_VIDEO_INFO_WIDTH(&info));
      const int height = static_cast<int>(GST_VIDEO_INFO_HEIGHT(&info));
      const int stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
      if (width > 0 && height > 0 && stride >= width * 3 &&
          map.size >= static_cast<size_t>(stride) * height &&
          !context->tile->frame_pending_.exchange(true)) {
        const QImage image(map.data, width, height, stride, QImage::Format_RGB888);
        const QImage copy = image.copy();
        QMetaObject::invokeMethod(context->tile,
                                  [tile = context->tile, generation = context->generation, copy] {
          tile->frame_pending_.store(false);
          if (generation != tile->generation_.load()) return;
          tile->frame_clock_.restart();
          tile->video_->setPixmap(QPixmap::fromImage(copy).scaled(
              tile->video_->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
          if (++tile->frames_ == 1) tile->set_status(QStringLiteral("正在播放"));
          if (tile->frame_callback_) tile->frame_callback_();
        }, Qt::QueuedConnection);
      }
      gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
  }

  void set_status(const QString& value) { status_->setText(QStringLiteral("状态：") + value); }

  void open_stream() {
    const QUrl source = QUrl::fromUserInput(url_->text().trimmed());
    if (!source.isValid() || source.scheme() != QStringLiteral("rtsp")) {
      wanted_ = false;
      set_status(QStringLiteral("请输入有效的 rtsp:// 地址"));
      return;
    }
    stop_pipeline();
    frames_ = 0;
    frame_clock_.restart();
    video_->setText(QStringLiteral("正在连接"));
    set_status(QStringLiteral("连接中：%1").arg(source.toString()));
    GError* error = nullptr;
    constexpr const char* kPipeline =
        "rtspsrc name=source protocols=tcp latency=200 "
        "source. ! application/x-rtp,media=video ! rtph264depay ! h264parse ! "
        "avdec_h264 ! videoconvert ! videoscale ! "
        "video/x-raw,format=RGB,width=960,height=540 ! "
        "appsink name=video_sink sync=false max-buffers=2 drop=true "
        "source. ! application/x-rtp,media=audio ! rtpmp4gdepay ! aacparse ! "
        "avdec_aac ! audioconvert ! audioresample ! "
        "volume name=audio_volume ! autoaudiosink sync=false";
    pipeline_ = gst_parse_launch(kPipeline, &error);
    if (error) {
      set_status(QStringLiteral("管线错误：%1").arg(QString::fromUtf8(error->message)));
      g_error_free(error);
      stop_pipeline();
      schedule_retry();
      return;
    }
    if (!pipeline_) {
      set_status(QStringLiteral("无法创建播放管线"));
      schedule_retry();
      return;
    }
    GstElement* input = gst_bin_get_by_name(GST_BIN(pipeline_), "source");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline_), "video_sink");
    volume_ = gst_bin_get_by_name(GST_BIN(pipeline_), "audio_volume");
    if (!input || !sink || !volume_) {
      if (input) gst_object_unref(input);
      if (sink) gst_object_unref(sink);
      set_status(QStringLiteral("播放组件缺失"));
      schedule_retry();
      return;
    }
    const QByteArray uri = source.toString(QUrl::FullyEncoded).toUtf8();
    g_object_set(input, "location", uri.constData(), nullptr);
    gst_object_unref(input);
    auto* context = new CallbackContext{this, generation_.load()};
    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = &StreamTile::new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, context,
                               [](gpointer data) { delete static_cast<CallbackContext*>(data); });
    gst_object_unref(sink);
    update_volume();
    bus_ = gst_element_get_bus(pipeline_);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      set_status(QStringLiteral("无法启动播放"));
      schedule_retry();
      return;
    }
    watch_->start();
  }

  void update_volume() {
    if (volume_) g_object_set(volume_, "volume",
                              muted_ ? 0.0 : volume_percent_ / 100.0, nullptr);
  }

  void stop_pipeline() {
    ++generation_;
    watch_->stop();
    if (pipeline_) {
      gst_element_set_state(pipeline_, GST_STATE_NULL);
      gst_object_unref(pipeline_);
      pipeline_ = nullptr;
    }
    if (bus_) { gst_object_unref(bus_); bus_ = nullptr; }
    if (volume_) { gst_object_unref(volume_); volume_ = nullptr; }
  }

  void poll_bus() {
    if (!bus_) return;
    while (GstMessage* message = gst_bus_pop(bus_)) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        set_status(QStringLiteral("播放错误：%1；3 秒后重连")
                       .arg(error ? QString::fromUtf8(error->message) : QStringLiteral("未知")));
        if (error) g_error_free(error);
        g_free(debug);
        gst_message_unref(message);
        schedule_retry();
        return;
      }
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        gst_message_unref(message);
        set_status(QStringLiteral("流已结束，3 秒后重连"));
        schedule_retry();
        return;
      }
      gst_message_unref(message);
    }
  }

  void schedule_retry() {
    if (!wanted_ || retry_->isActive()) return;
    stop_pipeline();
    retry_->start();
  }

  QLabel* video_;
  QLineEdit* url_;
  QLabel* status_;
  QTimer* retry_;
  QTimer* watch_;
  QElapsedTimer frame_clock_;
  std::function<void()> frame_callback_;
  GstElement* pipeline_ = nullptr;
  GstBus* bus_ = nullptr;
  GstElement* volume_ = nullptr;
  std::atomic<uint64_t> generation_{0};
  std::atomic<bool> frame_pending_{false};
  int volume_percent_ = 45;
  int frames_ = 0;
  bool muted_ = false;
  bool wanted_ = false;
};

}  // namespace

int main(int argc, char** argv) {
  gst_init(nullptr, nullptr);
  QApplication application(argc, argv);
  application.setApplicationName(QStringLiteral("Edge AV Qt Viewer"));
  QString url = QString::fromUtf8(kDefaultUrl);
  bool probe_mode = false;
  int timeout_seconds = 35;
  const QStringList arguments = application.arguments();
  for (int i = 1; i < arguments.size(); ++i) {
    if (arguments[i] == QStringLiteral("--probe")) probe_mode = true;
    else if (arguments[i] == QStringLiteral("--url") && i + 1 < arguments.size())
      url = arguments[++i];
    else if (arguments[i] == QStringLiteral("--timeout") && i + 1 < arguments.size())
      timeout_seconds = arguments[++i].toInt();
    else {
      std::cerr << "Usage: veg_qt_viewer [--url RTSP_URL] [--probe] [--timeout SECONDS]\n";
      return 2;
    }
  }
  if (timeout_seconds < 1 || timeout_seconds > 300) return 2;

  QMainWindow window;
  auto* center = new QWidget(&window);
  auto* grid = new QGridLayout(center);
  // One tile owns one RTSP pipeline; the grid can hold more tiles later.
  auto* tile = new StreamTile(url, center);
  grid->addWidget(tile, 0, 0);
  window.setCentralWidget(center);
  window.setWindowTitle(QStringLiteral("Edge AV 放行流预览（单路）"));
  window.resize(1050, 700);
  if (probe_mode) {
    int frames = 0;
    tile->on_frame([&] {
      if (++frames >= 30) {
        std::cout << "PROBE_OK frames=" << frames << std::endl;
        application.exit(0);
      }
    });
    QTimer::singleShot(timeout_seconds * 1000, &application, [&] {
      std::cerr << "PROBE_TIMEOUT frames=" << frames << std::endl;
      application.exit(2);
    });
  } else window.show();
  tile->start();
  return application.exec();
}
