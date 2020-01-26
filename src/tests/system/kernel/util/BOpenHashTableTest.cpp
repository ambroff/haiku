#include <vector>

#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>
#include <TestUtils.h>

#include <util/OpenHashTable.h>
#include <shared/AutoDeleter.h>

#include "BOpenHashTableTest.h"

namespace {

class Entry {
public:
	Entry(uint32_t value)
		:
		fValue(value),
		fNext(NULL)
	{
	}

	const uint32_t Value() const
	{
		return fValue;
	}

	Entry* Next() const
	{
		return fNext;
	}

private:
	uint32_t fValue;
	Entry* fNext;

	friend class EntryDefinition;
};


class EntryDefinition {
public:
	typedef uint32_t KeyType;
	typedef Entry ValueType;

	size_t HashKey(const KeyType& key) const
	{
		return key;
	}

	size_t Hash(Entry* entry) const
	{
		return entry->fValue;
	}

	bool Compare(const KeyType& key, Entry* entry) const
	{
		return key == entry->fValue;
	}

	Entry*& GetLink(Entry* entry) const
	{
		return entry->fNext;
	}
};

}


CppUnit::Test* BOpenHashTableTest::Suite()
{
	CppUnit::TestSuite* suite = new CppUnit::TestSuite("BOpenHashTable");

	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Insert test",
					   &BOpenHashTableTest::InsertTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Iterate and count test",
					   &BOpenHashTableTest::IterateAndCountTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Lookup test",
					   &BOpenHashTableTest::LookupTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Resize test",
					   &BOpenHashTableTest::ResizeTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Remove test",
					   &BOpenHashTableTest::RemoveTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Duplicate insert test",
					   &BOpenHashTableTest::DuplicateInsertTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Disable auto expand",
					   &BOpenHashTableTest::DisableAutoExpandTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Init with zero size",
					   &BOpenHashTableTest::InitWithZeroSizeTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Clear test",
					   &BOpenHashTableTest::ClearTest));
	suite->addTest(new CppUnit::TestCaller<BOpenHashTableTest>(
					   "BOpenHashTable::Clear and return test",
					   &BOpenHashTableTest::ClearAndReturnTest));

	return suite;
}


BOpenHashTableTest::BOpenHashTableTest(std::string name)
	: BTestCase(name)
{
}


void BOpenHashTableTest::InsertTest()
{
	Entry* entry = new Entry(123);
	ObjectDeleter<Entry> _(entry);

	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init() == B_OK);

	CHK(table.Insert(entry) == B_OK);
}


void BOpenHashTableTest::IterateAndCountTest() {
	const size_t kEntryCount = 20;

	std::vector<ObjectDeleter<Entry>> entries(kEntryCount);

	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init(kEntryCount * 2) == B_OK);

	for (uint32_t i = 0; i < kEntryCount; ++i) {
		Entry* entry = new Entry(i);
		entries[i].SetTo(entry);
		CHK(table.Insert(entry) == B_OK);
	}

	uint32_t sum = 0;
	uint32_t count = 0;

	BOpenHashTable<EntryDefinition>::Iterator iterator = table.GetIterator();
	while (iterator.HasNext()) {
		Entry* entry = iterator.Next();
		sum += entry->Value();
		++count;
	}

	CHK(count == table.CountElements());
	CHK(sum = 45);
}


void BOpenHashTableTest::ResizeTest()
{
	// This is the same as IterateAndCountTest, except that the table will
	// be resized mid insertion.
	const size_t kEntryCount = 20;
	std::vector<ObjectDeleter<Entry>> entries(kEntryCount);
	BOpenHashTable<EntryDefinition> table;

	// Start off with capacity for 8 elements. This will mean that the table
	// will be resized in the loop below since we are inserting 10 elements.
	CHK(table.Init(8) == B_OK);

	for (uint32_t i = 0; i < kEntryCount; ++i) {
		Entry* entry = new Entry(i);
		entries[i].SetTo(entry);
		CHK(table.Insert(entry) == B_OK);
	}

	// Verify that after resize the expected elements are present within
	// the table.
	uint32_t sum = 0;
	uint32_t count = 0;

	BOpenHashTable<EntryDefinition>::Iterator iterator = table.GetIterator();
	while (iterator.HasNext()) {
		Entry* entry = iterator.Next();
		sum += entry->Value();
		++count;
	}

	CHK(count == table.CountElements());
	CHK(sum = 45);
}


void BOpenHashTableTest::LookupTest() {
	Entry* entry = new Entry(123);
	ObjectDeleter<Entry> _(entry);

	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init(0) == B_OK);

	CHK(table.Insert(entry) == B_OK);
	CHK(table.Lookup(123) == entry);
}


void BOpenHashTableTest::RemoveTest() {
	Entry* entry = new Entry(123);
	ObjectDeleter<Entry> _(entry);

	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init(0) == B_OK);

	CHK(table.Insert(entry) == B_OK);
	CHK(table.Lookup(123) == entry);

	table.Remove(entry);
	CHK(table.Lookup(123) == NULL);
}


void BOpenHashTableTest::DuplicateInsertTest()
{
	Entry* entry = new Entry(123);
	ObjectDeleter<Entry> _(entry);

	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init(0) == B_OK);

	CHK(table.Insert(entry) == B_OK);
	CHK(table.Lookup(123) == entry);

	CHK(table.Insert(entry) == B_OK);
	CHK(table.Lookup(123) == entry);

	table.Remove(entry);
	CHK(table.Lookup(123) == NULL);
}


void BOpenHashTableTest::DisableAutoExpandTest()
{
	// Insert multiple items into a table with a fixed size of 1. This
	// essentially turns this BOpenHashTable into a linked list, since resize
	// will never occur.
	ObjectDeleter<Entry> entry1(new Entry(123));
	ObjectDeleter<Entry> entry2(new Entry(456));

	BOpenHashTable<EntryDefinition, false> table;
	CHK(table.Init(1) == B_OK);

	CHK(table.Insert(entry1.Get()) == B_OK);
	CHK(table.Insert(entry2.Get()) == B_OK);
	CHK(table.CountElements() == 2);
}


void BOpenHashTableTest::InitWithZeroSizeTest()
{
	Entry* entry = new Entry(123);
	ObjectDeleter<Entry> _(entry);

	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init(0) == B_OK);

	CHK(table.Insert(entry) == B_OK);
	CHK(table.Lookup(123) == entry);
}


void BOpenHashTableTest::ClearTest()
{
	const size_t kEntryCount = 3;
	
	std::vector<ObjectDeleter<Entry>> entries(kEntryCount);

	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init() == B_OK);

	for (uint32_t i = 0; i < kEntryCount; ++i) {
		Entry* entry = new Entry(i);
		entries[i].SetTo(entry);
		CHK(table.Insert(entry) == B_OK);
	}

	CHK(table.CountElements() == kEntryCount);
	CHK(table.Lookup(2) != NULL);

	CHK(table.Clear(false) == NULL);
	CHK(table.CountElements() == 0);
	CHK(table.Lookup(2) == NULL);
	CHK(table.GetIterator().HasNext() == false);
}


void BOpenHashTableTest::ClearAndReturnTest()
{
	// Same as ClearTest(), except that Clear(true) is called, which tells
	// the BOpenHashTable<T> to return a linked list of entries before clearing
	// the table.
	const size_t kEntryCount = 3;
	BOpenHashTable<EntryDefinition> table;
	CHK(table.Init() == B_OK);

	for (uint32_t i = 0; i < kEntryCount; ++i) {
		Entry* entry = new Entry(i);
		CHK(table.Insert(entry) == B_OK);
	}

	CHK(table.CountElements() == kEntryCount);
	CHK(table.Lookup(2) != NULL);

	Entry* head = table.Clear(true);
	CHK(head != NULL);

	CHK(table.CountElements() == 0);
	CHK(table.Lookup(2) == NULL);
	CHK(table.GetIterator().HasNext() == false);

	size_t items_returned = 0;
	while (head != NULL) {
		Entry* next = head->Next();
		delete head;
		head = next;

		++items_returned;
	}

	CHK(items_returned == kEntryCount);
}
