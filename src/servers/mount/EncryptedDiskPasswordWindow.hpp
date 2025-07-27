/*
 * Copyright 2025, Haiku Project. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Kyle Ambroff-Kao, kyle@ambroffkao.com
 */
#pragma once

#include <String.h>
#include <Window.h>


class BTextControl;
class BButton;
class BStringView;


class EncryptedDiskPasswordWindow : public BWindow {
public:
	EncryptedDiskPasswordWindow();
	virtual ~EncryptedDiskPasswordWindow();

	virtual bool QuitRequested();
	virtual void MessageReceived(BMessage* message);
	virtual void Show();

	status_t RequestPassword(const BString& partitionPath, const BString& partitionName,
		BString& password, const BString& errorMessage = BString());

private:
	void _SetUp(const BString& partitionPath, const BString& partitionName,
		const BString& errorMessage);

private:
	BStringView* fPartitionLabel;
	BStringView* fPartitionNameLabel;
	BStringView* fErrorLabel;
	BTextControl* fPasswordControl;
	BButton* fCancelButton;
	BButton* fUnlockButton;
	sem_id fDoneSem;
	status_t fResult;
	BString* fPassword;
};
