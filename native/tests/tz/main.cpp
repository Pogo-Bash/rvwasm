// The local time zone as Qt resolves it on linux-wasm (musl): QTimeZone, QDateTime, and the QML engine's JavaScript
// Date (getHours, getTimezoneOffset), which Fuzzy Clock uses. Run with TZ=America/Chicago.
#include <QCoreApplication>
#include <QDateTime>
#include <QJSEngine>
#include <QTimeZone>
#include <cstdio>
#include <ctime>

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const QDateTime now = QDateTime::currentDateTime();
	std::printf("tz: TZ=%s systemTimeZoneId=%s valid=%d\n", getenv("TZ") ? getenv("TZ") : "(unset)",
		QTimeZone::systemTimeZoneId().constData(), QTimeZone::systemTimeZone().isValid());
	std::printf("tz: QDateTime local %s utc %s offset %d\n", qPrintable(now.toString(Qt::ISODate)),
		qPrintable(now.toUTC().toString(Qt::ISODate)), now.offsetFromUtc());
	time_t t = time(nullptr);
	struct tm lt;
	localtime_r(&t, &lt);
	std::printf("tz: libc localtime_r hour %d isdst %d gmtoff %ld zone %s\n", lt.tm_hour, lt.tm_isdst, lt.tm_gmtoff, lt.tm_zone);
	QJSEngine js;
	std::printf("tz: JS getHours %s getTimezoneOffset %s\n",
		qPrintable(js.evaluate("new Date().getHours()").toString()),
		qPrintable(js.evaluate("new Date().getTimezoneOffset()").toString()));
	std::printf("tz: JS from QDateTime hours %s\n", qPrintable(js.toScriptValue(now).property("getHours").callWithInstance(js.toScriptValue(now)).toString()));
	return 0;
}
