// Stage (b) of docs/native-wasm.md: a QWidget window on the machine's Wayland desktop.
// Prints how long it took from main() to the window's first paint, then quits after
// QTEST_SECONDS (default 0: stays), saying the frames drawn (QTEST_ANIMATE=1: as fast as it can), the CPU
// time used meanwhile, and the memory /proc/self/status gives.
#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QElapsedTimer>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <cstdio>
#include <cstdlib>
#include <sys/times.h>
#include <unistd.h>

static QElapsedTimer since_main;
static clock_t cpu0;

class Canvas : public QWidget {
public:
	int frames = 0;
	bool animate = false;
protected:
	void paintEvent(QPaintEvent *) override {
		if (frames++ == 0)
			std::printf("qwidget: first paint %lld ms after main, at %lld ms (epoch)\n", (long long)since_main.elapsed(),
				(long long)QDateTime::currentMSecsSinceEpoch());
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(rect(), QColor(0x3a, 0x5a, 0x40));
		const int n = 24;
		for (int i = 0; i < n; i++) {
			p.setPen(QPen(QColor::fromHsv((i * 15 + frames * 3) % 360, 160, 230), 3));
			p.drawEllipse(QPointF(width() / 2.0, height() / 2.0), 10.0 + i * 6, 10.0 + i * 4);
		}
		p.setPen(Qt::white);
		p.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignLeft | Qt::AlignTop,
			QStringLiteral("native Qt %1 on linux-wasm, frame %2").arg(qVersion()).arg(frames));
		if (animate)
			update();
	}
};

int main(int argc, char **argv) {
	since_main.start();
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	std::printf("qwidget: main\n");
	QApplication app(argc, argv);
	std::printf("qwidget: QApplication %lld ms, platform %s\n", (long long)since_main.elapsed(),
		qPrintable(QGuiApplication::platformName()));
	QWidget w;
	w.setWindowTitle(QStringLiteral("Native Qt"));
	auto *layout = new QVBoxLayout(&w);
	auto *label = new QLabel(QStringLiteral("Hello from Qt %1 compiled to WebAssembly").arg(qVersion()));
	auto *canvas = new Canvas;
	canvas->setMinimumSize(360, 240);
	auto *button = new QPushButton(QStringLiteral("Animate"));
	QObject::connect(button, &QPushButton::clicked, [canvas] { canvas->animate = !canvas->animate; canvas->update(); });
	layout->addWidget(label);
	layout->addWidget(canvas);
	layout->addWidget(button);
	w.resize(420, 340);
	w.show();
	if (getenv("QTEST_ANIMATE"))
		canvas->animate = true;
	const int seconds = getenv("QTEST_SECONDS") ? atoi(getenv("QTEST_SECONDS")) : 0;
	QElapsedTimer run;
	if (seconds > 0)
		QTimer::singleShot(seconds * 1000, [&] {
			struct tms t;
			times(&t);
			const double cpu = double(t.tms_utime + t.tms_stime - cpu0) * 1000.0 / sysconf(_SC_CLK_TCK);
			std::printf("qwidget: %d frames in %lld ms (%.1f fps), %.0f ms of CPU (%.1f%%)\n", canvas->frames,
				(long long)run.elapsed(), canvas->frames * 1000.0 / run.elapsed(), cpu, cpu * 100.0 / run.elapsed());
			QFile status(QStringLiteral("/proc/self/status"));
			if (status.open(QIODevice::ReadOnly))
				for (const QByteArray &line : status.readAll().split('\n'))
					if (line.startsWith("VmRSS") || line.startsWith("VmSize") || line.startsWith("VmPeak") || line.startsWith("Threads"))
						std::printf("qwidget: %s\n", line.simplified().constData());
			// On this kernel (no MMU) status has no Vm lines; statm's first number is every mapping the
			// process has, in pages (task_nommu.c).
			QFile statm(QStringLiteral("/proc/self/statm"));
			if (statm.open(QIODevice::ReadOnly))
				std::printf("qwidget: statm %s (mapped %.1f MiB)\n", statm.readAll().simplified().constData(),
					statm.seek(0) ? statm.readAll().split(' ').value(0).toDouble() * 4096 / 1048576 : 0.0);
			app.quit();
		});
	struct tms t0;
	times(&t0);
	cpu0 = t0.tms_utime + t0.tms_stime;
	run.start();
	return app.exec();
}
