// QtCore alone on linux-wasm: event loop, timers, a QThread with a queued signal, strings, files.
// Each step prints a line; the last one is "qcore: ok".
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv) {
	QElapsedTimer t;
	t.start();
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	std::printf("qcore: main\n");
	QCoreApplication app(argc, argv);
	std::printf("qcore: QCoreApplication %lld ms\n", (long long)t.elapsed());
	std::printf("qcore: %s, %lld entries in /\n", qPrintable(QString::fromUtf8("Qt %1 on %2").arg(qVersion(), QSysInfo::kernelType())),
		(long long)QDir(QStringLiteral("/")).entryList().size());
	QThread worker;
	QObject in_worker;
	in_worker.moveToThread(&worker);
	worker.start();
	QTimer::singleShot(0, &in_worker, [&] {
		std::printf("qcore: in the thread\n");
		QTimer::singleShot(0, &app, [&] {
			std::printf("qcore: back in main, %lld ms\n", (long long)t.elapsed());
			QTimer::singleShot(50, &app, [&] { app.quit(); });
		});
	});
	int r = app.exec();
	worker.quit();
	worker.wait();
	std::printf("qcore: ok %lld ms\n", (long long)t.elapsed());
	return r;
}
