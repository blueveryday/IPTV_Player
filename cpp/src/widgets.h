// 自绘控件：SeekBar / PanelToggle / VSplitter，以及全局深色样式表
#pragma once

#include <QWidget>
#include <functional>

namespace iptv {

QString darkStyleSheet();

class SeekBar : public QWidget {
    Q_OBJECT
public:
    explicit SeekBar(QWidget *parent = nullptr);
    void setValue(double v);          // 0..1000
    double value() const { return m_value; }
    bool isDragging() const { return m_dragging; }
    bool isSeekEnabled() const { return m_enabled; }
    void setSeekEnabled(bool en);
    QSize sizeHint() const override { return {200, 18}; }

signals:
    void seek(double value, const QString &phase); // phase: "preview" / "commit"

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    double xToValue(int x) const;
    double m_value = 0;
    bool m_dragging = false;
    bool m_enabled = false;
};

class PanelToggle : public QWidget {
    Q_OBJECT
public:
    explicit PanelToggle(QWidget *parent = nullptr);
    QSize sizeHint() const override { return {7, 46}; }

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void enterEvent(QEnterEvent *) override { m_hover = true; update(); }
    void leaveEvent(QEvent *) override { m_hover = false; update(); }

private:
    bool m_hover = false;
};

class VSplitter : public QWidget {
    Q_OBJECT
public:
    explicit VSplitter(QWidget *parent = nullptr);

signals:
    void pressed();
    void dragged(int dx);
    void released();

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    bool m_active = false;
    int m_lastX = 0;
};

} // namespace iptv
