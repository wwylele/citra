#include <QDockWidget>
#include <QSlider>
#include "core/frontend/emu_window.h"
class EmuThread;

class StereoscopicControllerWidget : public QDockWidget {
    Q_OBJECT

public:
    StereoscopicControllerWidget(QWidget* parent = nullptr);

public slots:
    void OnEmulationStarting(EmuThread* emu_thread);
    void OnEmulationStopping();

    void OnSliderMoved(int);
signals:
    void DepthChanged(float value);
    void StereoscopeModeChanged(EmuWindow::StereoscopicMode mode);

private:
    QSlider* slider;
};