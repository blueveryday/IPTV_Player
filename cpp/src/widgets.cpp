#include "widgets.h"

#include "util.h"

#include <QMouseEvent>
#include <QPainter>

namespace iptv {

using namespace Colors;

QString darkStyleSheet() {
    // 对应 Python 版 _setup_ttk_style（clam 主题 + COLORS）
    QString s;
    s += QString(R"(
QWidget { background-color: %1; color: %2; }
QMainWindow, QDialog { background-color: %1; }
QLabel { background: transparent; color: %2; }
QLabel#dim { color: %3; }
QLabel#sidebarDim { background: %4; color: %3; }
QLabel#status { background: %5; color: %3; padding: 3px 6px; }
QLabel#rec { background: %5; color: %6; padding: 3px 6px; }
QFrame#sidebar, QWidget#sidebar { background-color: %4; }
QFrame#panel, QWidget#panel { background-color: %7; }
QFrame#toolbar { background-color: %7; }
QFrame#statusBar { background-color: %5; }
QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox {
    background-color: %8; color: %2; border: 1px solid %9;
    selection-background-color: %10; selection-color: %11; padding: 2px 3px; }
QComboBox { background-color: %8; color: %2; border: 1px solid %9; padding: 2px 6px;
    selection-background-color: %8; selection-color: %2; }
QComboBox::drop-down { border: none; width: 18px; background: %12; }
QComboBox::down-arrow { image: url(:/arrow_down.png); width: 11px; height: 7px; }
QComboBox QAbstractItemView { background-color: %8; color: %2; border: 1px solid %9;
    selection-background-color: %10; selection-color: %11; outline: 0; }
QPushButton { background-color: %12; color: %2; border: 1px solid %9; padding: 4px 8px; }
QPushButton:hover { background-color: %13; }
QPushButton:pressed { background-color: %14; }
QPushButton:disabled { color: #888888; }
QPushButton#recActive { color: %6; font-weight: bold; }
QPushButton#stopActive { color: %15; font-weight: bold; }
QPushButton#muted { color: %6; font-weight: bold; }
QPushButton#disabledBtn { color: #888888; }
QCheckBox { background: transparent; color: %2; spacing: 6px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid %9; background: %8; }
QCheckBox::indicator:checked { background: %16; }
QListWidget, QListView { background-color: %4; color: %2; border: none; outline: 0; }
QListWidget::item:selected, QListView::item:selected { background-color: %10; color: %11; }
QScrollBar:vertical { background: %1; width: 12px; border: none; margin: 0; }
QScrollBar::handle:vertical { background: %12; min-height: 20px; border: 1px solid %1; }
QScrollBar::handle:vertical:hover { background: %13; }
QScrollBar:horizontal { background: %1; height: 12px; border: none; margin: 0; }
QScrollBar::handle:horizontal { background: %12; min-width: 20px; border: 1px solid %1; }
QScrollBar::handle:horizontal:hover { background: %13; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: %1; }
QSlider::groove:horizontal { height: 6px; background: %9; }
QSlider::sub-page:horizontal { background: %16; }
QSlider::handle:horizontal { background: %7; border: 2px solid %16; width: 10px; margin: -5px 0; border-radius: 7px; }
QGroupBox { background: %7; border: 1px solid %9; margin-top: 8px; padding-top: 6px; }
QGroupBox::title { subcontrol-origin: margin; left: 8px; color: %3; background: %7; }
QMenuBar { background-color: %7; color: %2; }
QMenuBar::item { background: transparent; padding: 4px 8px; }
QMenuBar::item:selected { background-color: %17; }
QMenu { background-color: %7; color: %2; border: 1px solid %9; }
QMenu::item { padding: 4px 24px 4px 22px; }
QMenu::item:selected { background-color: %17; color: #ffffff; }
QMenu::item:disabled { color: %18; }
QMenu::separator { height: 1px; background: %9; margin: 3px 0; }
QMenu::indicator { width: 13px; height: 13px; }
QToolTip { background-color: %8; color: %2; border: 1px solid %9; }
QProgressBar { border: 1px solid %9; background: %8; text-align: center; color: %2; }
QProgressBar::chunk { background: %16; }
QTabWidget::pane { border: 1px solid %9; }
QTableWidget { background-color: %4; color: %2; gridline-color: %9; }
QHeaderView::section { background-color: %12; color: %2; border: 1px solid %9; padding: 2px 4px; }
)")
             .arg(bg_root, fg_primary, fg_secondary, bg_video, status_bg, rec_red,
                  bg_panel, bg_input, border, select_bg, select_fg, btn_bg,
                  btn_active, accent_dim, live_green, accent, accent_dim)
             .arg(disabled_fg);
    return s;
}

// ===================================================================== SeekBar
SeekBar::SeekBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(18);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAutoFillBackground(false);
}

void SeekBar::setValue(double v) {
    v = qBound(0.0, v, 1000.0);
    if (v == m_value) return;
    m_value = v;
    update();
}

void SeekBar::setSeekEnabled(bool en) {
    if (en == m_enabled) return;
    m_enabled = en;
    setCursor(en ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

double SeekBar::xToValue(int x) const {
    if (width() <= 1) return 0;
    return qBound(0.0, x * 1000.0 / width(), 1000.0);
}

void SeekBar::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(bg_panel));
    const int w = width(), h = height();
    if (w <= 1 || h <= 1) return;
    const int cy = h / 2;
    p.fillRect(QRect(0, cy - 3, w, 6), QColor(border));
    const int fw = int(w * m_value / 1000.0);
    if (fw > 0) p.fillRect(QRect(0, cy - 3, fw, 6), QColor(accent));
    const int tx = qMax(6, qMin(w - 6, fw));
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(QColor(bg_panel));
    p.setPen(QPen(QColor(accent), 2));
    p.drawEllipse(QPointF(tx, cy), 6, 6);
}

void SeekBar::mousePressEvent(QMouseEvent *e) {
    if (!m_enabled || e->button() != Qt::LeftButton) return;
    m_dragging = true;
    const double v = xToValue(int(e->position().x()));
    setValue(v);
    emit seek(v, "preview");
}

void SeekBar::mouseMoveEvent(QMouseEvent *e) {
    if (!m_dragging) return;
    const double v = xToValue(int(e->position().x()));
    setValue(v);
    emit seek(v, "preview");
}

void SeekBar::mouseReleaseEvent(QMouseEvent *e) {
    if (!m_dragging) return;
    m_dragging = false;
    const double v = xToValue(int(e->position().x()));
    setValue(v);
    emit seek(v, "commit");
}

// ===================================================================== PanelToggle
PanelToggle::PanelToggle(QWidget *parent) : QWidget(parent) {
    setFixedSize(7, 46);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
}

void PanelToggle::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(bg_video));
    const int cx = width() / 2, cy = height() / 2;
    p.setPen(QPen(QColor(m_hover ? accent : fg_secondary), 2));
    p.drawLine(cx - 2, cy - 9, cx - 2, cy + 9);
    p.drawLine(cx + 2, cy - 9, cx + 2, cy + 9);
}

void PanelToggle::mousePressEvent(QMouseEvent *e) {
    if (e->button() == Qt::LeftButton) emit clicked();
}

// ===================================================================== VSplitter
VSplitter::VSplitter(QWidget *parent) : QWidget(parent) {
    setFixedWidth(6);
    setCursor(Qt::SplitHCursor);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(bg_video));
    setPalette(pal);
}

void VSplitter::mousePressEvent(QMouseEvent *e) {
    if (e->button() != Qt::LeftButton) return;
    m_active = true;
    m_lastX = int(e->globalPosition().x());
    emit pressed();
}

void VSplitter::mouseMoveEvent(QMouseEvent *e) {
    if (!m_active) return;
    const int x = int(e->globalPosition().x());
    const int dx = x - m_lastX;
    m_lastX = x;
    if (dx) emit dragged(dx);
}

void VSplitter::mouseReleaseEvent(QMouseEvent *) {
    if (m_active) emit released();
    m_active = false;
}

} // namespace iptv
