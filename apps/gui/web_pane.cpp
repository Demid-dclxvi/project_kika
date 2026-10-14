// Веб-интерфейс принтера. Qt WebEngine (LGPL-3.0, отдельные библиотеки) — если сборка с ним
// (KIKA_WITH_WEBENGINE), иначе ссылка на браузер.

#include "web_pane.hpp"

#include <QDesktopServices>
#include <QLabel>
#include <QPushButton>
#include <QStackedLayout>
#include <QVBoxLayout>

#ifdef KIKA_WITH_WEBENGINE
#include <QWebEngineView>
#endif

#include "widgets.hpp"

namespace kika::gui {

WebPane::WebPane(QWidget* parent) : QWidget(parent) {
  setObjectName("webPane");
  stack_ = new QStackedLayout(this);
  placeholder_ = new QWidget;
  placeholder_->setObjectName("webPlaceholder");
  auto* v = new QVBoxLayout(placeholder_);
  v->setContentsMargins(40, 40, 40, 40);
  v->addStretch();
  text_ = make_label("", "muted");
  text_->setAlignment(Qt::AlignCenter);
  text_->setTextFormat(Qt::RichText);
  v->addWidget(text_);
  open_ = make_button("Открыть в браузере", "primary");
  open_->hide();
  v->addWidget(open_, 0, Qt::AlignHCenter);
  v->addStretch();
  stack_->addWidget(placeholder_);
  connect(open_, &QPushButton::clicked, this, [this] {
    if (!url_.isEmpty()) QDesktopServices::openUrl(QUrl(url_));
  });
  show_placeholder();
}

bool WebPane::embedded() {
#ifdef KIKA_WITH_WEBENGINE
  return true;
#else
  return false;
#endif
}

QWidget* WebPane::content() const { return view_ && stack_->currentWidget() == view_ ? view_ : placeholder_; }

void WebPane::show_placeholder() {
  if (url_.isEmpty()) {
    text_->setText("<b>Веб-интерфейс принтера</b><br><br>Подключите принтер слева — здесь откроется его страница "
                   "(Fluidd или Mainsail): камера, консоль, файлы, настройки Klipper.");
    open_->hide();
  } else {
    text_->setText("<b>Веб-интерфейс принтера</b><br><br>" + url_.toHtmlEscaped() +
                   "<br><br>Эта сборка — без встроенного браузера (Qt WebEngine): страница принтера откроется в "
                   "обычном браузере.");
    open_->show();
  }
  stack_->setCurrentWidget(placeholder_);
}

void WebPane::set_url(const QString& url) {
  if (url == url_ && (view_ || url.isEmpty())) return;
  url_ = url;
  loaded_ = false;
#ifdef KIKA_WITH_WEBENGINE
  if (!url_.isEmpty()) {
    if (!view_) {
      auto* web = new QWebEngineView;
      view_ = web;
      stack_->addWidget(web);
      connect(web, &QWebEngineView::loadFinished, this, [this](bool ok) { loaded_ = ok; });
    }
    static_cast<QWebEngineView*>(view_)->load(QUrl(url_));
    stack_->setCurrentWidget(view_);
    return;
  }
#endif
  show_placeholder();
}

}  // namespace kika::gui
