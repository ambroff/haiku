/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Kyle Ambroff-Kao, kyle@ambroffkao.com
 */
#include "EncryptedDiskPasswordWindow.hpp"

#include <Button.h>
#include <Catalog.h>
#include <GridLayout.h>
#include <GridView.h>
#include <GroupLayout.h>
#include <GroupView.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>

#include <new>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "EncryptedDiskPasswordWindow"


static const uint32 kMessageCancel = 'btcl';
static const uint32 kMessageUnlock = 'btul';


EncryptedDiskPasswordWindow::EncryptedDiskPasswordWindow()
	:
	BWindow(BRect(50, 50, 100, 100), B_TRANSLATE("Unlock encrypted disk"), B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	fPartitionLabel(NULL),
	fPartitionNameLabel(NULL),
	fErrorLabel(NULL),
	fPasswordControl(NULL),
	fCancelButton(NULL),
	fUnlockButton(NULL),
	fDoneSem(-1),
	fResult(B_ERROR),
	fPassword(NULL)
{
	fDoneSem = create_sem(0, "encrypted disk unlock dialog");
	if (fDoneSem < 0)
		return;

	float inset = be_plain_font->Size() * 0.7f;

	// Create the message view
	BTextView* message = new(std::nothrow) BTextView("message");
	message->SetText(
		B_TRANSLATE("The disk partition below is encrypted. "
					"Please enter the passphrase to unlock it.\n\n"
					"Once unlocked, the partition will remain accessible until the system "
					"is shut down or the partition is manually locked."));
	message->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	rgb_color textColor = ui_color(B_PANEL_TEXT_COLOR);
	message->SetFontAndColor(be_plain_font, B_FONT_ALL, &textColor);
	message->MakeEditable(false);
	message->MakeSelectable(false);
	message->SetWordWrap(true);

	// Create partition info section
	BStringView* partitionPathLabelView
		= new(std::nothrow) BStringView("partitionPathLabelView", B_TRANSLATE("Device:"));
	fPartitionLabel = new(std::nothrow) BStringView("partitionLabel", "");

	BStringView* partitionNameLabelView
		= new(std::nothrow) BStringView("partitionNameLabelView", B_TRANSLATE("Name:"));
	fPartitionNameLabel = new(std::nothrow) BStringView("partitionNameLabel", "");

	// Create error message label (initially hidden)
	fErrorLabel = new(std::nothrow) BStringView("errorLabel", "");
	fErrorLabel->SetHighUIColor(B_FAILURE_COLOR);
	fErrorLabel->Hide();

	// Create password control
	fPasswordControl = new(std::nothrow) BTextControl(B_TRANSLATE("Passphrase:"), "", NULL);
	fPasswordControl->TextView()->HideTyping(true);

	// Set minimum width for password field
	BSize minSize(fPasswordControl->StringWidth("0123456789012345678901234567890123456789") + inset,
		B_SIZE_UNSET);

	// Create buttons
	fCancelButton = new(std::nothrow) BButton(B_TRANSLATE("Cancel"), new BMessage(kMessageCancel));
	fUnlockButton = new(std::nothrow) BButton(B_TRANSLATE("Unlock"), new BMessage(kMessageUnlock));

	// Build layout manually
	BGroupLayout* rootLayout = new(std::nothrow) BGroupLayout(B_VERTICAL);
	SetLayout(rootLayout);
	rootLayout->SetInsets(inset, inset, inset, inset);
	rootLayout->SetSpacing(inset);

	// Set window background to match system panel color
	BView* rootView = rootLayout->View();
	if (rootView)
		rootView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	rootLayout->AddView(message);

	// Create grid for partition info and password
	BGridView* grid = new(std::nothrow) BGridView();
	BGridLayout* gridLayout = grid->GridLayout();
	gridLayout->SetSpacing(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING);

	gridLayout->AddView(partitionPathLabelView, 0, 0);
	gridLayout->AddView(fPartitionLabel, 1, 0);
	gridLayout->AddView(partitionNameLabelView, 0, 1);
	gridLayout->AddView(fPartitionNameLabel, 1, 1);
	gridLayout->AddView(fErrorLabel, 0, 2, 2, 1); // Span across 2 columns
	gridLayout->AddItem(fPasswordControl->CreateLabelLayoutItem(), 0, 3);
	BLayoutItem* textItem = fPasswordControl->CreateTextViewLayoutItem();
	textItem->SetExplicitMinSize(minSize);
	gridLayout->AddItem(textItem, 1, 3);

	rootLayout->AddView(grid);

	// Create button group
	BGroupView* buttonGroup = new(std::nothrow) BGroupView(B_HORIZONTAL);
	buttonGroup->GroupLayout()->AddView(fCancelButton);
	buttonGroup->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
	buttonGroup->GroupLayout()->AddView(fUnlockButton);

	rootLayout->AddView(buttonGroup);
}


EncryptedDiskPasswordWindow::~EncryptedDiskPasswordWindow()
{
	if (fDoneSem >= 0)
		delete_sem(fDoneSem);
}


bool
EncryptedDiskPasswordWindow::QuitRequested()
{
	fResult = B_CANCELED;
	release_sem(fDoneSem);
	return false;
}


void
EncryptedDiskPasswordWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMessageCancel:
		case kMessageUnlock:
			fResult = message->what == kMessageUnlock ? B_OK : B_CANCELED;
			release_sem(fDoneSem);
			return;
	}

	BWindow::MessageReceived(message);
}


status_t
EncryptedDiskPasswordWindow::RequestPassword(const BString& partitionPath,
	const BString& partitionName, BString& password, const BString& errorMessage)
{
	_SetUp(partitionPath, partitionName, errorMessage);
	fPassword = &password;

	ResizeToPreferred();
	CenterOnScreen();
	Show(); // This will call our overridden Show() which sets up the controls

	while (acquire_sem(fDoneSem) == B_INTERRUPTED)
		;

	status_t result = fResult;
	if (result == B_OK)
		*fPassword = fPasswordControl->Text();

	// Clear password from control
	if (Lock()) {
		fPasswordControl->SetText("");
		Unlock();
	}

	LockLooper();
	Quit();
	return result;
}


void
EncryptedDiskPasswordWindow::Show()
{
	BWindow::Show();

	// Set up controls after window is shown
	if (Lock()) {
		fCancelButton->SetTarget(this);
		fUnlockButton->SetTarget(this);
		fUnlockButton->MakeDefault(true);
		fPasswordControl->MakeFocus();
		Unlock();
	}
}


void
EncryptedDiskPasswordWindow::_SetUp(const BString& partitionPath, const BString& partitionName,
	const BString& errorMessage)
{
	fPartitionLabel->SetText(partitionPath);

	// Display partition name, or a fallback if empty
	if (partitionName.IsEmpty())
		fPartitionNameLabel->SetText(B_TRANSLATE("(unnamed)"));
	else
		fPartitionNameLabel->SetText(partitionName);

	// Display error message if provided
	if (!errorMessage.IsEmpty()) {
		fErrorLabel->SetText(errorMessage);
		fErrorLabel->Show();
	} else {
		fErrorLabel->Hide();
	}
}
