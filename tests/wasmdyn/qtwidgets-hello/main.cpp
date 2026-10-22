// A QMainWindow with a QLabel and a QPushButton against the shared Qt libraries (Core, Gui, Widgets NEEDED; the Wayland
// platform plugin, xdg-shell and the decoration plugin dlopen'ed by Qt). Runs on the window server; prints what it did.
//   qtwidgets-hello [SECONDS]      (default 8: then quits)
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cstdio>

class Window : public QMainWindow {
	Q_OBJECT
public:
	Window() {
		auto *page = new QWidget;
		auto *box = new QVBoxLayout(page);
		label = new QLabel("Hello from shared Qt Widgets");
		label->setAlignment(Qt::AlignCenter);
		QFont f = label->font();
		f.setPointSize(18);
		label->setFont(f);
		button = new QPushButton("Click me");
		box->addWidget(label);
		box->addWidget(button);
		setCentralWidget(page);
		setWindowTitle("qtwidgets-hello");
		connect(button, &QPushButton::clicked, this, &Window::onClicked);
	}
	QLabel *label;
	QPushButton *button;
	int clicks = 0;
public slots:
	void onClicked() {
		clicks++;
		label->setText(QString("clicked %1 time%2").arg(clicks).arg(clicks == 1 ? "" : "s"));
		std::printf("qtwidgets: clicked %d\n", clicks);
	}
};

static long mappedKiB() {
	QFile f("/proc/self/maps");
	long total = 0;
	if (f.open(QIODevice::ReadOnly)) {
		for (const QByteArray &line : f.readAll().split('\n')) {
			int dash = line.indexOf('-'), sp = line.indexOf(' ');
			if (dash > 0 && sp > dash)
				total += (line.left(sp).mid(dash + 1).toULong(nullptr, 16) - line.left(dash).toULong(nullptr, 16)) / 1024;
		}
	}
	return total;
}

int main(int argc, char **argv) {
	QElapsedTimer t;
	t.start();
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	int seconds = argc > 1 ? std::atoi(argv[1]) : 8;
	std::printf("qtwidgets: main\n");
	QApplication app(argc, argv);
	std::printf("qtwidgets: QApplication %lld ms, platform %s\n", (long long)t.elapsed(), qPrintable(QGuiApplication::platformName()));
	Window w;
	w.resize(420, 200);
	w.show();
	std::printf("qtwidgets: shown %lld ms\n", (long long)t.elapsed());
	QTimer::singleShot(0, &app, [&] { std::printf("qtwidgets: event loop running %lld ms\n", (long long)t.elapsed()); });
	QTimer::singleShot(2500, &w, [&] { w.button->click(); });
	QTimer::singleShot(seconds * 1000, &app, [&] {
		std::printf("qtwidgets: mappings %ld KiB\n", mappedKiB());
		app.quit();
	});
	int rc = app.exec();
	std::printf("qtwidgets: done rc=%d clicks=%d %lld ms\n", rc, w.clicks, (long long)t.elapsed());
	return rc;
}

#include "main.moc"
