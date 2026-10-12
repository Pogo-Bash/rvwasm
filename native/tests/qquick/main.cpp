// Stage (d) of docs/native-wasm.md: a Qt Quick window (QML interpreted, no JIT) on the machine's Wayland
// desktop. Prints the time from main() to the first frame, and with QTEST_SECONDS the frames shown in that
// time, the CPU used and the process's mappings. QSG_RHI_BACKEND / QT_QUICK_BACKEND choose the renderer
// (opengl through Mesa's EGL; software).
#include <QDateTime>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QSGRendererInterface>
#include <QQuickWindow>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <sys/times.h>
#include <atomic>
#include <csignal>
#include <pthread.h>
#include <thread>
#include <unistd.h>


// SIGUSR1 traps: on linux-wasm the runtime then prints the Wasm stack of the thread that took it, which
// shows where a hung program waits (kill -USR1 PID).
static void trap_on_signal(int) { __builtin_trap(); }

int main(int argc, char **argv)
{
	if (getenv("QTEST_TRAP_ON_USR1") || getenv("QTEST_WATCHDOG"))
		std::signal(SIGUSR1, trap_on_signal);
	// QTEST_WATCHDOG=S: when the main thread's event loop has not run for S seconds, send it SIGUSR1 (its stack
	// is then printed by the runtime, if it is in a system call to be interrupted).
	static std::atomic<long long> heartbeat{0};
	static pthread_t main_thread = pthread_self();
	if (getenv("QTEST_WATCHDOG")) {
		const int limit = atoi(getenv("QTEST_WATCHDOG"));
		std::thread([limit] {
			QElapsedTimer t;
			t.start();
			long long last = -1, since = 0;
			for (;;) {
				usleep(500000);
				long long h = heartbeat.load();
				if (h != last) { last = h; since = t.elapsed(); continue; }
				if (t.elapsed() - since > limit * 1000) {
					struct tms tt;
					times(&tt);
					std::fprintf(stderr, "qquick: main thread stuck for %d s: SIGUSR1 (process CPU %ld ticks)\n", limit,
						(long)(tt.tms_utime + tt.tms_stime));
					pthread_kill(main_thread, SIGUSR1);
					since = t.elapsed();
				}
			}
		}).detach();
	}
	QElapsedTimer since_main;
	since_main.start();
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	QGuiApplication app(argc, argv);
	app.setApplicationVersion(QStringLiteral(QT_VERSION_STR));
	std::printf("qquick: QGuiApplication %lld ms, platform %s\n", (long long)since_main.elapsed(),
		qPrintable(QGuiApplication::platformName()));
	QQuickView view;
	view.setTitle(QStringLiteral("Native Qt Quick"));
	view.setResizeMode(QQuickView::SizeRootObjectToView);
	const bool animate = getenv("QTEST_ANIMATE") != nullptr;
	view.setSource(QUrl(QStringLiteral("qrc:/main.qml")));
	for (const auto &e : view.errors())
		std::printf("qquick: %s\n", qPrintable(e.toString()));
	view.rootObject()->setProperty("animate", animate);
	int frames = 0;
	QObject::connect(&view, &QQuickWindow::frameSwapped, [&] {
		if (frames++ == 0)
			std::printf("qquick: first frame %lld ms after main, at %lld ms (epoch), graphics api %d (1 software, 3 OpenGL)\n",
				(long long)since_main.elapsed(), (long long)QDateTime::currentMSecsSinceEpoch(),
				int(view.rendererInterface()->graphicsApi()));
	});
	view.show();
	QTimer beat;
	QObject::connect(&beat, &QTimer::timeout, [] { heartbeat++; });
	beat.start(100);
	const int seconds = getenv("QTEST_SECONDS") ? atoi(getenv("QTEST_SECONDS")) : 0;
	QElapsedTimer run;
	struct tms t0;
	times(&t0);
	run.start();
	if (seconds > 0)
		QTimer::singleShot(seconds * 1000, [&] {
			struct tms t;
			times(&t);
			const double cpu = double(t.tms_utime + t.tms_stime - t0.tms_utime - t0.tms_stime) * 1000.0
				/ sysconf(_SC_CLK_TCK);
			std::printf("qquick: %d frames in %lld ms (%.1f fps), %.0f ms of CPU (%.1f%%)\n", frames,
				(long long)run.elapsed(), frames * 1000.0 / run.elapsed(), cpu, cpu * 100.0 / run.elapsed());
			app.quit();
		});
	return app.exec();
}
