// Веб-интерфейс принтера. Qt WebEngine (LGPL-3.0, отдельные библиотеки) — если сборка с ним
// (KIKA_WITH_WEBENGINE), иначе ссылка на браузер.

#include "web_pane.hpp"

#include <QDesktopServices>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QStackedLayout>
#include <QVBoxLayout>

#ifdef KIKA_WITH_WEBENGINE
#include <QWebEngineHistory>
#include <QWebEngineView>
#endif

#include "widgets.hpp"

namespace kika::gui {

WebPane::WebPane(QWidget* parent) : QWidget(parent) {
  setObjectName("webPane");
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  // строка над страницей: адрес и кнопки
  bar_ = new QFrame;
  bar_->setObjectName("pickBar");
  {
    auto* b = new QHBoxLayout(bar_);
    b->setContentsMargins(12, 6, 12, 6);
    b->setSpacing(6);
    back_ = make_button("←", "sm");
    back_->setToolTip("Назад");
    reload_ = make_button("Обновить", "sm");
    address_ = make_label("", "muted", false);
    address_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    external_ = make_button("В браузере", "sm");
    external_->setToolTip("Открыть страницу принтера в обычном браузере");
    b->addWidget(back_);
    b->addWidget(reload_);
    b->addWidget(address_, 1);
    b->addWidget(external_);
  }
  bar_->hide();
  outer->addWidget(bar_);
  auto* body = new QWidget;
  stack_ = new QStackedLayout(body);
  outer->addWidget(body, 1);
  placeholder_ = new QWidget;
  placeholder_->setObjectName("webPlaceholder");
  auto* v = new QVBoxLayout(placeholder_);
  v->setContentsMargins(40, 40, 40, 40);
  v->addStretch();
  text_ = make_label("", "muted");
  text_->setAlignment(Qt::AlignCenter);
  text_->setTextFormat(Qt::RichText);
  v->addWidget(text_);
  auto* row = hbox(8);
  row->addStretch();
  retry_ = make_button("Повторить", "sm");
  retry_->hide();
  open_ = make_button("Открыть в браузере", "primary");
  open_->hide();
  row->addWidget(retry_);
  row->addWidget(open_);
  row->addStretch();
  v->addLayout(row);
  v->addStretch();
  stack_->addWidget(placeholder_);
  auto open_external = [this] {
    if (!url_.isEmpty()) QDesktopServices::openUrl(QUrl(url_));
  };
  connect(open_, &QPushButton::clicked, this, open_external);
  connect(external_, &QPushButton::clicked, this, open_external);
  connect(retry_, &QPushButton::clicked, this, [this] { load(); });
  connect(reload_, &QPushButton::clicked, this, [this] { load(); });
#ifdef KIKA_WITH_WEBENGINE
  connect(back_, &QPushButton::clicked, this, [this] {
    if (view_) static_cast<QWebEngineView*>(view_)->back();
  });
#endif
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

void WebPane::show_placeholder(const QString& error) {
  retry_->setVisible(!error.isEmpty());
  if (url_.isEmpty()) {
    text_->setText("<b>Веб-интерфейс принтера</b><br><br>Подключите принтер слева — здесь откроется его страница "
                   "(Fluidd или Mainsail): камера, консоль, файлы, настройки Klipper.");
    open_->hide();
  } else if (!error.isEmpty()) {
    text_->setText("<b>Страница принтера не открылась</b><br><br>" + url_.toHtmlEscaped() + "<br><br>" + error.toHtmlEscaped());
    open_->show();
  } else {
    text_->setText("<b>Веб-интерфейс принтера</b><br><br>" + url_.toHtmlEscaped() +
                   "<br><br>Эта сборка — без встроенного браузера (Qt WebEngine): страница принтера откроется в "
                   "обычном браузере.");
    open_->show();
  }
  stack_->setCurrentWidget(placeholder_);
}

void WebPane::load() {
  loaded_ = false;
#ifdef KIKA_WITH_WEBENGINE
  if (!url_.isEmpty()) {
    if (!view_) {
      auto* web = new QWebEngineView;
      view_ = web;
      stack_->addWidget(web);
      connect(web, &QWebEngineView::loadFinished, this, [this, web](bool ok) {
        loaded_ = true;
        back_->setEnabled(web->history()->canGoBack());
        if (!ok)
          show_placeholder("Проверьте, что принтер включён и в той же сети, и что веб-интерфейс (Fluidd, Mainsail) у него "
                           "открыт на этом адресе.");
        else
          stack_->setCurrentWidget(web);
      });
      connect(web, &QWebEngineView::urlChanged, this, [this](const QUrl& u) { address_->setText(u.toString()); });
    }
    static_cast<QWebEngineView*>(view_)->load(QUrl(url_));
    stack_->setCurrentWidget(view_);
    return;
  }
#endif
  show_placeholder();
}

void WebPane::set_url(const QString& url) {
  if (url == url_ && (view_ || url.isEmpty())) return;
  url_ = url;
  address_->setText(url_);
  bar_->setVisible(embedded() && !url_.isEmpty());
  back_->setEnabled(false);
  load();
}

}  // namespace kika::gui
