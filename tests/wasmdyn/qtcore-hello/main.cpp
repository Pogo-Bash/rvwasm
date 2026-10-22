// A Qt console program against libQt6Core.so (NEEDED, not dlopen): the first Qt program through ld-wasm.
// Strings, a timer firing a slot through a signal, a QThread that signals back to the main thread, quit.
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <cstdio>

class Worker : public QThread {
	Q_OBJECT
public:
	explicit Worker(QObject *main) : mainObj(main) {}
	void run() override {
		QStringList l = QString("a,b,c").split(',');
		emit result(l.size() * 14, QThread::currentThread() != mainObj->thread());
	}
signals:
	void result(int, bool);
private:
	QObject *mainObj;
};

class Hub : public QObject {
	Q_OBJECT
public:
	int ticks = 0, fromThread = 0;
	QElapsedTimer sinceThreadStart;
public slots:
	void tick() {
		std::printf("tick %d\n", ++ticks);
		if (ticks == 1) {
			auto *t = new Worker(this);
			QObject::connect(t, &Worker::result, this, &Hub::fromWorker);
			QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
			sinceThreadStart.start();
			t->start();
		}
	}
	void fromWorker(int v, bool workerWasOffMain) {
		std::printf("thread result %d (ran off the main thread: %s, delivered on main: %s, thread start to delivery %lld ms)\n", v,
			workerWasOffMain ? "yes" : "no", QThread::currentThread() == thread() ? "yes" : "no", (long long)sinceThreadStart.elapsed());
		fromThread = v;
		QCoreApplication::quit();
	}
};

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	std::printf("Qt %s\n", qVersion());
	QString s = QString("Hello, %1!").arg("Qt on wasm");
	std::printf("%s upper=%s len=%d idx=%d\n", qPrintable(s), qPrintable(s.toUpper()), (int)s.size(), (int)s.indexOf("Qt"));
	QStringList l = QString("pear,apple,fig,banana").split(',');
	l.sort();
	std::printf("sorted=%s joined=%s\n", qPrintable(l.join(" ")), qPrintable(l.join("+")));
	std::printf("utf8 round trip=%s number=%s\n", qPrintable(QString::fromUtf8("h\xc3\xa9llo").toUpper()), qPrintable(QString::number(3.14159, 'f', 2)));
	Hub hub;
	QTimer timer;
	timer.setInterval(20);
	QObject::connect(&timer, &QTimer::timeout, &hub, &Hub::tick);
	timer.start();
	int rc = app.exec();
	std::printf("done rc=%d ticks>=1:%s threadValue=%d\n", rc, hub.ticks >= 1 ? "yes" : "no", hub.fromThread);
	return rc;
}
#include "main.moc"
