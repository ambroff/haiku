// AlertTest.cpp
#include "AlertTest.h"
#include <cppunit/Test.h>
#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>
#include <stdio.h>
#include <string.h>
#include <Application.h>
#include <Alert.h>
#include <Point.h>
#include <TextView.h>
#include <Button.h>
#include <Rect.h>

#include <iostream>
using std::ostream;

#define ASSERT_DEQUAL(x,y) CPPUNIT_ASSERT_DOUBLES_EQUAL((x),(y),0.01)

const char *k20X = "XXXXXXXXXXXXXXXXXXXX";
const char *k40X = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";
const char *k60X = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";

// Required by CPPUNIT_ASSERT_EQUAL(rgb_color, rgb_color)
#if TEST_R5
bool operator==(const rgb_color &left, const rgb_color &right)
{
	if (left.red == right.red && left.green == right.green &&
	    left.blue == right.blue && left.alpha == right.alpha)
	    return true;
	else
		return false;
}
#endif	// TEST_R5

// Required by CPPUNIT_ASSERT_EQUAL(rgb_color, rgb_color)
ostream &operator<<(ostream &stream, const rgb_color &clr)
{
	return stream << "rgb_color(" << clr.red << ", " <<
		clr.green << ", " << clr.blue << ", " << clr.alpha << ")";
}

// For storing expected state of windows or views.
struct GuiInfo {
	const char*	label;
};

// For storing all the information required to create and
// verify the state of a BAlert object.
class AlertTestInfo {
public:
	AlertTestInfo(AlertTest *pTest);
	~AlertTestInfo();
	
	void SetWinInfo(const GuiInfo &winInfo);
	void SetTextViewInfo(const GuiInfo &textInfo);
	void SetButtonInfo(int32 btn, const GuiInfo &btnInfo);
	
	void SetButtonWidthMode(button_width widthMode);
	void SetButtonSpacingMode(button_spacing spacingMode);
	void SetAlertType(alert_type alertType);
	
	void GuiInfoTest();

private:
	AlertTest*		fTest;
	GuiInfo			fWinInfo;
	GuiInfo			fTextInfo;
	GuiInfo			fButtonInfo[3];
	int32 			fButtonCount;
	button_width	fWidthMode;
	button_spacing	fSpacingMode;
	alert_type		fAlertType;
};

AlertTestInfo::AlertTestInfo(AlertTest *pTest)
{
	memset(this, 0, sizeof(AlertTestInfo));
	
	fTest = pTest;
	fWidthMode = B_WIDTH_AS_USUAL;
	fSpacingMode = B_EVEN_SPACING;
	fAlertType = B_INFO_ALERT;
}

AlertTestInfo::~AlertTestInfo()
{
	fTest = NULL;
	fButtonCount = 0;
}

void
AlertTestInfo::SetWinInfo(const GuiInfo &winInfo)
{
	fWinInfo = winInfo;
}

void
AlertTestInfo::SetTextViewInfo(const GuiInfo &textInfo)
{
	fTextInfo = textInfo;
}

void
AlertTestInfo::SetButtonInfo(int32 btn, const GuiInfo &btnInfo)
{
	if (btn < 0 || btn > 2 || btn > fButtonCount) {
		CPPUNIT_ASSERT(false);
		return;
	}
	fButtonInfo[btn] = btnInfo;
	if (btn == fButtonCount && fButtonCount < 3)
		fButtonCount++;
}
	
void
AlertTestInfo::SetButtonWidthMode(button_width widthMode)
{
	fWidthMode = widthMode;
}

void
AlertTestInfo::SetButtonSpacingMode(button_spacing spacingMode)
{
	fSpacingMode = spacingMode;
}

void
AlertTestInfo::SetAlertType(alert_type alertType)
{
	fAlertType = alertType;
}
	
void
AlertTestInfo::GuiInfoTest()
{
	fTest->NextSubTest();
	// Dummy application object required to create Window objects.
	BApplication app("application/x-vnd.Haiku-interfacekit_alerttest");
	BAlert *pAlert = new BAlert(
		fWinInfo.label,
		fTextInfo.label,
		fButtonInfo[0].label,
		fButtonInfo[1].label,
		fButtonInfo[2].label,
		fWidthMode,
		fSpacingMode,
		fAlertType
	);
	CPPUNIT_ASSERT(pAlert);

	// Alert Window Width/Height
	fTest->NextSubTest();
	ASSERT_DEQUAL(100.0f, pAlert->Bounds().Width());
	ASSERT_DEQUAL(100.0f, pAlert->Bounds().Height());
	
	// [k] MasterView
	fTest->NextSubTest();
	BView *masterView = pAlert->ChildAt(0);
	CPPUNIT_ASSERT(masterView);
	
	// [k] MasterView Color
	fTest->NextSubTest();
	CPPUNIT_ASSERT_EQUAL(ui_color(B_PANEL_BACKGROUND_COLOR),
		masterView->ViewColor());

	// Test all the buttons
	BButton *btns[3] = { NULL };
	for (int32 i = 0; i < 3; i++) {
		fTest->NextSubTest();
		btns[i] = pAlert->ButtonAt(i);
		
		if (i >= fButtonCount || fButtonInfo[i].label[0] == '\0') {
			// If there is should be no button at this index
			CPPUNIT_ASSERT_EQUAL((BButton*)NULL, btns[i]);
		} else {
			// If there should be a button at this index
			CPPUNIT_ASSERT(btns[i]);

			CPPUNIT_ASSERT(
				strcmp(fButtonInfo[i].label, btns[i]->Label()) == 0);
				
			ASSERT_DEQUAL(-1.0f, btns[i]->Bounds().Width());
			
			ASSERT_DEQUAL(-1.0f, btns[i]->Bounds().Height());
			
			BPoint pt = btns[i]->ConvertToParent(BPoint(0, 0));
			ASSERT_DEQUAL(0.0f, pt.x);
			ASSERT_DEQUAL(0.0f, pt.y);
			
			if (i == fButtonCount - 1) {
				// Default button
				CPPUNIT_ASSERT_EQUAL(true, btns[i]->IsDefault());
			}
		}
	}

	// [k] TextView
	fTest->NextSubTest();
	BTextView *textView = pAlert->TextView();
	CPPUNIT_ASSERT(textView);

	// [k] TextView ViewColor()
	fTest->NextSubTest();
	CPPUNIT_ASSERT_EQUAL(ui_color(B_PANEL_BACKGROUND_COLOR), textView->ViewColor());

	// [k] TextView IsEditable()
	fTest->NextSubTest();
	CPPUNIT_ASSERT_EQUAL(false, textView->IsEditable());

	// [k] TextView IsSelectable()
	fTest->NextSubTest();
	CPPUNIT_ASSERT_EQUAL(false, textView->IsSelectable());

	// [k] TextView DoesWordWrap()
	fTest->NextSubTest();
	CPPUNIT_ASSERT_EQUAL(true, textView->DoesWordWrap());

	// TextView Text
	fTest->NextSubTest();
	CPPUNIT_ASSERT(strcmp(fTextInfo.label, textView->Text()) == 0);

	// TextView Width/Height
	fTest->NextSubTest();
	ASSERT_DEQUAL(-1.0f, textView->Bounds().Width());
	ASSERT_DEQUAL(-1.0f, textView->Bounds().Height());

	// TextView Position
	fTest->NextSubTest();
	BPoint pt = textView->ConvertToParent(BPoint(0, 0));
	ASSERT_DEQUAL(0.0f, pt.x);
	ASSERT_DEQUAL(0.0f, pt.y);

	// Now we can show the alert which will compute the actual layout of the
	// alert view. We don't check the exact size or position of any of the
	// views inside of the alert, aside from making sure that those values
	// have changed to some non-default value. Checking for absolute pixel
	// sizes doesn't work since things like font, font size and locale can
	// change the final layout.
	fTest->NextSubTest();
	int32 buttonPressed = pAlert->Go(new BInvoker());
	CPPUNIT_ASSERT_EQUAL(0, buttonPressed);

	bool locked = pAlert->Lock();
	CPPUNIT_ASSERT_EQUAL(true, locked);

	fTest->NextSubTest();
	CPPUNIT_ASSERT(pAlert->Bounds().Width() > 100.0);
	CPPUNIT_ASSERT(pAlert->Bounds().Height() > 100.0);

	fTest->NextSubTest();
	for (int32_t i = 0; i < fButtonCount; ++i) {
		if (btns[i] != NULL) {
			CPPUNIT_ASSERT(btns[i]->Bounds().Width() > 0.0);
			CPPUNIT_ASSERT(btns[i]->Bounds().Height() > 0.0);

			BPoint pt = btns[i]->ConvertToParent(BPoint(0, 0));
			CPPUNIT_ASSERT(pt.x > 0.0);
			CPPUNIT_ASSERT(pt.y > 0.0);
		}
	}

	fTest->NextSubTest();
	CPPUNIT_ASSERT(textView->Bounds().Width() > 0.0);
	CPPUNIT_ASSERT(textView->Bounds().Height() > 0.0);

	fTest->NextSubTest();
	pt = textView->ConvertToParent(BPoint(0, 0));
	CPPUNIT_ASSERT(pt.x > 0.0);
	CPPUNIT_ASSERT(pt.y > 0.0);
}

// Suite
CppUnit::Test *
AlertTest::Suite()
{
	CppUnit::TestSuite *suite = new CppUnit::TestSuite();
	typedef CppUnit::TestCaller<AlertTest> TC;
	
#define AT_ADDTEST(fn) (suite->addTest(new TC("Alert " #fn, &AlertTest::fn)))
	
	////// UW_ES_IA - One Button //////
	AT_ADDTEST(empty_empty_UW_ES_IA);
	AT_ADDTEST(OK_X_UW_ES_IA);
	AT_ADDTEST(OK_60X_UW_ES_IA);
	AT_ADDTEST(twentyX_60X_UW_ES_IA);
	AT_ADDTEST(fortyX_60X_UW_ES_IA);
	
	// ////// LW_ES_IA - One Button //////
	AT_ADDTEST(empty_empty_LW_ES_IA);
	AT_ADDTEST(OK_X_LW_ES_IA);
	AT_ADDTEST(twentyX_60X_LW_ES_IA);
	AT_ADDTEST(fortyX_60X_LW_ES_IA);
	
	////// WW_ES_IA - One Button //////
	AT_ADDTEST(empty_empty_WW_ES_IA);
	AT_ADDTEST(OK_X_WW_ES_IA);
	AT_ADDTEST(twentyX_60X_WW_ES_IA);
	
	////// UW_ES_EA - One Button //////
	AT_ADDTEST(OK_X_UW_ES_EA);
	AT_ADDTEST(fortyX_60X_UW_ES_EA);

	////// UW_OS_IA - One Button //////
	AT_ADDTEST(OK_X_UW_OS_IA);
	AT_ADDTEST(fortyX_60X_UW_OS_IA);
	
	////// LW_OS_IA - One Button //////
	AT_ADDTEST(OK_X_LW_OS_IA);
	
	////// UW_OS_EA - One Button //////
	AT_ADDTEST(OK_X_UW_OS_EA);

	////// UW_ES_IA - Two Button //////
	AT_ADDTEST(OK_Cancel_60X_UW_ES_IA);
	AT_ADDTEST(twentyX_Cancel_60X_UW_ES_IA);
	AT_ADDTEST(twentyX_20X_60X_UW_ES_IA);
	
	////// LW_ES_IA - Two Button //////
	AT_ADDTEST(empty_empty_X_LW_ES_IA);
	AT_ADDTEST(OK_Cancel_60X_LW_ES_IA);
	
	////// WW_ES_IA - Two Button //////
	AT_ADDTEST(empty_empty_X_WW_ES_IA);
	AT_ADDTEST(OK_Cancel_60X_WW_ES_IA);
	AT_ADDTEST(twentyX_Cancel_60X_WW_ES_IA);
	AT_ADDTEST(twentyX_20X_WW_ES_IA);
	
	////// UW_ES_EA - Two Button //////
	AT_ADDTEST(OK_Cancel_60X_UW_ES_EA);
	AT_ADDTEST(twentyX_20X_60X_UW_ES_EA);
	
	////// UW_OS_IA - Two Button //////
	AT_ADDTEST(OK_Cancel_60X_UW_OS_IA);
	
	////// LW_OS_IA - Two Button //////
	AT_ADDTEST(OK_Cancel_60X_LW_OS_IA);
	
	////// LW_OS_EA - Two Button //////
	AT_ADDTEST(twentyX_OK_60X_LW_OS_EA);
			
	////// UW_ES_IA - Three Button //////
	AT_ADDTEST(twentyX_20X_20X_60X_UW_ES_IA);
	
	////// LW_ES_IA - Three Button //////
	AT_ADDTEST(empty_empty_empty_X_LW_ES_IA);
	AT_ADDTEST(Yes_No_Cancel_X_LW_ES_IA);
	AT_ADDTEST(twentyX_20X_20X_60X_LW_ES_IA);
	
	////// WW_ES_IA - Three Button //////
	AT_ADDTEST(empty_empty_empty_X_WW_ES_IA);
	AT_ADDTEST(Monkey_Dog_Cat_X_WW_ES_IA);
	AT_ADDTEST(X_20X_X_WW_ES_IA);
	AT_ADDTEST(Yes_No_Cancel_X_WW_ES_IA);
	AT_ADDTEST(twentyX_20X_20X_60X_WW_ES_IA);
	
	////// UW_ES_EA - Three Button //////
	AT_ADDTEST(twentyX_20X_20X_60X_UW_ES_EA);
	
	////// UW_OS_IA - Three Button //////
	AT_ADDTEST(Yes_No_Cancel_60X_UW_OS_IA);
	
	////// LW_OS_IA - Three Button //////
	AT_ADDTEST(Yes_No_Cancel_60X_LW_OS_IA);
	
	////// WW_OS_IA - Three Button //////
	AT_ADDTEST(Monkey_Dog_Cat_60X_WW_OS_IA);
	
	////// UW_OS_EA - Three Button //////
	AT_ADDTEST(twentyX_OK_Cancel_60X_UW_OS_EA);
		
	return suite;
}		

// setUp
void
AlertTest::setUp()
{
	BTestCase::setUp();
}
	
// tearDown
void
AlertTest::tearDown()
{
	BTestCase::tearDown();
}

////// UW_ES_IA - One Button //////

void
AlertTest::empty_empty_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "";
	ati.SetTextViewInfo(ti);
	
	bi.label = "";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::OK_X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::OK_60X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_60X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::fortyX_60X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k40X;
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// LW_ES_IA - One Button //////

void
AlertTest::empty_empty_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "";
	ati.SetTextViewInfo(ti);
	
	bi.label = "";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::OK_X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_60X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::fortyX_60X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k40X;
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// WW_ES_IA - One Button //////

void
AlertTest::empty_empty_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = "";
	ati.SetTextViewInfo(ti);
	
	bi.label = "";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::OK_X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_60X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// UW_ES_EA - One Button //////

void
AlertTest::OK_X_UW_ES_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::fortyX_60X_UW_ES_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k40X;
	ati.SetButtonInfo(0, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}

////// UW_OS_IA - One Button //////

void
AlertTest::OK_X_UW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
		
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::fortyX_60X_UW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k40X;
	ati.SetButtonInfo(0, bi);
		
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}
	
////// LW_OS_IA - One Button //////

void
AlertTest::OK_X_LW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
		
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}
	
////// UW_OS_EA - One Button //////

void
AlertTest::OK_X_UW_OS_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
		
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}

////// UW_ES_IA - Two Button //////

void
AlertTest::OK_Cancel_60X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_Cancel_60X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_20X_60X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// LW_ES_IA - Two Button //////

void
AlertTest::empty_empty_X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "";
	ati.SetButtonInfo(0, bi);
	bi.label = "";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::OK_Cancel_60X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// WW_ES_IA - Two Button //////

void
AlertTest::empty_empty_X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "";
	ati.SetButtonInfo(0, bi);
	bi.label = "";
	ati.SetButtonInfo(1, bi); 
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::OK_Cancel_60X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_Cancel_60X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void 
AlertTest::twentyX_20X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// UW_ES_EA - Two Button //////

void
AlertTest::OK_Cancel_60X_UW_ES_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_20X_60X_UW_ES_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}

////// UW_OS_IA - Two Button //////

void
AlertTest::OK_Cancel_60X_UW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}
	
////// LW_OS_IA - Two Button //////

void
AlertTest::OK_Cancel_60X_LW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "OK";
	ati.SetButtonInfo(0, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(1, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}
	
////// LW_OS_EA - Two Button //////

void
AlertTest::twentyX_OK_60X_LW_OS_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = "OK";
	ati.SetButtonInfo(1, bi);

	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}
	
////// UW_ES_IA - Three Button //////

void
AlertTest::twentyX_20X_20X_60X_UW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	bi.label = k20X;
	ati.SetButtonInfo(2, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// LW_ES_IA - Three Button //////

void
AlertTest::empty_empty_empty_X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "";
	ati.SetButtonInfo(0, bi);
	bi.label = "";
	ati.SetButtonInfo(1, bi);
	bi.label = "";
	ati.SetButtonInfo(2, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::Yes_No_Cancel_X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "Yes";
	ati.SetButtonInfo(0, bi);
	bi.label = "No";
	ati.SetButtonInfo(1, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(2, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void
AlertTest::twentyX_20X_20X_60X_LW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	bi.label = k20X;
	ati.SetButtonInfo(2, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

////// WW_ES_IA - Three Button //////

void 
AlertTest::empty_empty_empty_X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "";
	ati.SetButtonInfo(0, bi);
	bi.label = "";
	ati.SetButtonInfo(1, bi);
	bi.label = "";
	ati.SetButtonInfo(2, bi); 
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void 
AlertTest::Monkey_Dog_Cat_X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "Monkey";
	ati.SetButtonInfo(0, bi);
	bi.label = "Dog";
	ati.SetButtonInfo(1, bi);
	bi.label = "Cat";
	ati.SetButtonInfo(2, bi); 
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void 
AlertTest::X_20X_X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "X";
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	bi.label = "X";
	ati.SetButtonInfo(2, bi); 
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void 
AlertTest::Yes_No_Cancel_X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = "X";
	ati.SetTextViewInfo(ti);
	
	bi.label = "Yes";
	ati.SetButtonInfo(0, bi);
	bi.label = "No";
	ati.SetButtonInfo(1, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(2, bi); 
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}

void 
AlertTest::twentyX_20X_20X_60X_WW_ES_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	bi.label = k20X;
	ati.SetButtonInfo(2, bi); 
	
	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}


////// UW_ES_EA - Three Button //////

void
AlertTest::twentyX_20X_20X_60X_UW_ES_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = k20X;
	ati.SetButtonInfo(1, bi);
	bi.label = k20X;
	ati.SetButtonInfo(2, bi);
	
	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_EVEN_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}

////// UW_OS_IA - Three Button //////

void
AlertTest::Yes_No_Cancel_60X_UW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "Yes";
	ati.SetButtonInfo(0, bi);
	bi.label = "No";
	ati.SetButtonInfo(1, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(2, bi); 

	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}
	
////// LW_OS_IA - Three Button //////

void
AlertTest::Yes_No_Cancel_60X_LW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "Yes";
	ati.SetButtonInfo(0, bi);
	bi.label = "No";
	ati.SetButtonInfo(1, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(2, bi); 

	ati.SetButtonWidthMode(B_WIDTH_FROM_LABEL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}
	
////// WW_OS_IA - Three Button //////

void
AlertTest::Monkey_Dog_Cat_60X_WW_OS_IA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = "Monkey";
	ati.SetButtonInfo(0, bi);
	bi.label = "Dog";
	ati.SetButtonInfo(1, bi);
	bi.label = "Cat";
	ati.SetButtonInfo(2, bi); 

	ati.SetButtonWidthMode(B_WIDTH_FROM_WIDEST);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_INFO_ALERT);
	
	ati.GuiInfoTest();
}
	
////// UW_OS_EA - Three Button //////

void
AlertTest::twentyX_OK_Cancel_60X_UW_OS_EA()
{
	AlertTestInfo ati(this);
	GuiInfo wi, ti, bi;
	wi.label = "alert1";
	ati.SetWinInfo(wi);
	
	ti.label = k60X;
	ati.SetTextViewInfo(ti);
	
	bi.label = k20X;
	ati.SetButtonInfo(0, bi);
	bi.label = "OK";
	ati.SetButtonInfo(1, bi);
	bi.label = "Cancel";
	ati.SetButtonInfo(2, bi); 

	ati.SetButtonWidthMode(B_WIDTH_AS_USUAL);
	ati.SetButtonSpacingMode(B_OFFSET_SPACING);
	ati.SetAlertType(B_EMPTY_ALERT);
	
	ati.GuiInfoTest();
}

