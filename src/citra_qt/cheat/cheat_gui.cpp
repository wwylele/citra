#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QTableWidgetItem>
#include "citra_qt/cheat/cheat_gui.h"
#include "core/hle/kernel/process.h"
#include "ui_cheat_gui.h"

CheatDialog::CheatDialog(QWidget* parent)
    : QDialog(parent), ui(std::make_unique<Ui::CheatDialog>()) {
    // Setup gui control settings
    ui->setupUi(this);
    setWindowFlags(Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint);
    ui->tableCheats->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->tableCheats->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableCheats->setColumnWidth(0, 30);
    ui->tableCheats->setColumnWidth(2, 85);
    ui->tableCheats->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    ui->tableCheats->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->tableCheats->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    ui->textDetails->setEnabled(false);
    ui->textNotes->setEnabled(false);
    const auto game_id =
        Common::StringFromFormat("%016llX", Kernel::g_current_process->codeset->program_id);
    ui->labelTitle->setText(tr("Title ID: %1").arg(QString::fromStdString(game_id)));

    connect(ui->buttonClose, &QPushButton::released, this, &CheatDialog::OnCancel);
    connect(ui->buttonNewCheat, &QPushButton::released, this, &CheatDialog::OnAddCheat);
    connect(ui->buttonSave, &QPushButton::released, this, &CheatDialog::OnSave);
    connect(ui->buttonDelete, &QPushButton::released, this, &CheatDialog::OnDelete);
    connect(ui->tableCheats, &QTableWidget::cellClicked, this, &CheatDialog::OnRowSelected);
    connect(ui->textDetails, &QPlainTextEdit::textChanged, this, &CheatDialog::OnDetailsChanged);
    connect(ui->textNotes, &QPlainTextEdit::textChanged, this, &CheatDialog::OnNotesChanged);

    LoadCheats();
}

CheatDialog::~CheatDialog() {}

void CheatDialog::LoadCheats() {
    cheats = CheatEngine::CheatEngine::ReadFileContents();

    ui->tableCheats->setRowCount(cheats.size());

    for (size_t i = 0; i < cheats.size(); i++) {
        auto enabled = new QCheckBox();
        enabled->setChecked(cheats[i]->GetEnabled());
        enabled->setStyleSheet("margin-left:7px;");
        ui->tableCheats->setItem(i, 0, new QTableWidgetItem());
        ui->tableCheats->setCellWidget(i, 0, enabled);
        ui->tableCheats->setItem(
            i, 1, new QTableWidgetItem(QString::fromStdString(cheats[i]->GetName())));
        ui->tableCheats->setItem(
            i, 2, new QTableWidgetItem(QString::fromStdString(cheats[i]->GetType())));
        enabled->setProperty("row", static_cast<int>(i));

        connect(enabled, &QCheckBox::stateChanged, this, &CheatDialog::OnCheckChanged);
    }
}

void CheatDialog::OnSave() {
    CheatEngine::CheatEngine::Save(cheats);
    CheatCore::RefreshCheats();
    close();
}

void CheatDialog::OnCancel() {
    close();
}

void CheatDialog::OnRowSelected(int row, int column) {
    selection_changing = true;
    if (row == -1) {
        ui->textNotes->setPlainText("");
        ui->textDetails->setPlainText("");
        current_row = -1;
        selection_changing = false;
        ui->textDetails->setEnabled(false);
        ui->textNotes->setEnabled(false);
        return;
    }

    ui->textDetails->setEnabled(true);
    ui->textNotes->setEnabled(true);
    const auto& current_cheat = cheats[row];
    ui->textNotes->setPlainText(
        QString::fromStdString(Common::Join(current_cheat->GetNotes(), "\n")));

    std::vector<std::string> details;
    for (const auto& line : current_cheat->GetCheatLines())
        details.push_back(line.cheat_line);
    ui->textDetails->setPlainText(QString::fromStdString(Common::Join(details, "\n")));

    current_row = row;

    selection_changing = false;
}

void CheatDialog::OnNotesChanged() {
    if (selection_changing)
        return;
    auto notes = ui->textNotes->toPlainText();
    auto old_notes = cheats[current_row]->GetNotes();
    Common::SplitString(notes.toStdString(), '\n', old_notes);
    cheats[current_row]->SetNotes(old_notes);
}

void CheatDialog::OnDetailsChanged() {
    if (selection_changing)
        return;
    auto details = ui->textDetails->toPlainText();
    std::vector<std::string> detail_lines;
    Common::SplitString(details.toStdString(), '\n', detail_lines);
    auto new_lines = std::vector<CheatEngine::CheatLine>();
    for (const auto& line : detail_lines) {
        new_lines.emplace_back(line);
    }
    cheats[current_row]->SetCheatLines(new_lines);
}

void CheatDialog::OnCheckChanged(int state) {
    const QCheckBox* checkbox = qobject_cast<QCheckBox*>(sender());
    int row = static_cast<int>(checkbox->property("row").toInt());
    cheats[row]->SetEnabled(state);
}

void CheatDialog::OnDelete() {
    QItemSelectionModel* selectionModel = ui->tableCheats->selectionModel();
    QModelIndexList selected = selectionModel->selectedRows();
    std::vector<int> rows;
    for (int i = selected.count() - 1; i >= 0; i--) {
        QModelIndex index = selected.at(i);
        int row = index.row();
        cheats.erase(cheats.begin() + row);
        rows.push_back(row);
    }
    for (int row : rows)
        ui->tableCheats->removeRow(row);

    ui->tableCheats->clearSelection();
    OnRowSelected(-1, -1);
}

void CheatDialog::OnAddCheat() {
    NewCheatDialog dialog;
    dialog.exec();

    auto result = dialog.GetReturnValue();
    if (result == nullptr)
        return;
    cheats.push_back(result);
    int new_cheat_index = static_cast<int>(cheats.size() - 1);
    auto enabled = new QCheckBox();
    ui->tableCheats->setRowCount(cheats.size());
    enabled->setCheckState(Qt::CheckState::Unchecked);
    enabled->setStyleSheet("margin-left:7px;");
    ui->tableCheats->setItem(new_cheat_index, 0, new QTableWidgetItem());
    ui->tableCheats->setCellWidget(new_cheat_index, 0, enabled);
    ui->tableCheats->setItem(new_cheat_index, 1, new QTableWidgetItem(QString::fromStdString(
                                                     cheats[new_cheat_index]->GetName())));
    ui->tableCheats->setItem(new_cheat_index, 2, new QTableWidgetItem(QString::fromStdString(
                                                     cheats[new_cheat_index]->GetType())));
    enabled->setProperty("row", new_cheat_index);
    connect(enabled, &QCheckBox::stateChanged, this, &CheatDialog::OnCheckChanged);
    ui->tableCheats->selectRow(new_cheat_index);
    OnRowSelected(new_cheat_index, 0);
}
NewCheatDialog::NewCheatDialog(QWidget* parent) : QDialog(parent) {
    resize(250, 150);
    setSizeGripEnabled(false);
    auto mainLayout = new QVBoxLayout(this);

    QHBoxLayout* namePanel = new QHBoxLayout();
    name_block = new QLineEdit();
    QLabel* nameLabel = new QLabel();
    nameLabel->setText(tr("Name: "));
    namePanel->addWidget(nameLabel);
    namePanel->addWidget(name_block);

    QHBoxLayout* typePanel = new QHBoxLayout();
    auto typeLabel = new QLabel();
    typeLabel->setText(tr("Type: "));
    type_select = new QComboBox();
    type_select->addItem(tr("Gateway"), 0);
    typePanel->addWidget(typeLabel);
    typePanel->addWidget(type_select);

    auto button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(button_box, &QDialogButtonBox::accepted, this, [=]() {
        auto name = name_block->text().toStdString();
        if (type_select->currentIndex() == 0 && Common::Trim(name).length() > 0) {
            return_value =
                std::make_shared<CheatEngine::GatewayCheat>(name_block->text().toStdString());
        }
        this->close();
    });
    connect(button_box, &QDialogButtonBox::rejected, this, [=]() { this->close(); });
    QHBoxLayout* confirmationPanel = new QHBoxLayout();
    confirmationPanel->addWidget(button_box);
    mainLayout->addLayout(namePanel);
    mainLayout->addLayout(typePanel);
    mainLayout->addLayout(confirmationPanel);
}
NewCheatDialog::~NewCheatDialog() {}
