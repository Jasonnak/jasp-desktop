#include "jaspimporter.h"

#include <boost/foreach.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <sys/stat.h>

#include <fcntl.h>
#include "sharedmemory.h"
#include "dataset.h"

//#include "libzip/config.h"
#include "libzip/archive.h"
#include "libzip/archive_entry.h"
#include "../lib_json/json.h"

#include "filereader.h"
#include "tempfiles.h"

using namespace std;

bool JASPImporter::parseJsonEntry(Json::Value &root, const string &path,  const string &entry, bool required)
{
	FileReader dataEntry = FileReader(path, entry);

	if (!dataEntry.archiveExists())
		throw runtime_error("The selected JASP archive '" + path + "' does not exist.");

	if (!dataEntry.exists())
	{
		if (required)
			throw runtime_error("Entry " + entry + " could not be found in JASP archive.");

		return false;
	}

	int size = dataEntry.bytesAvailable();
	if (size > 0)
	{
		char data[size];
		int startOffset = dataEntry.pos();
		int errorCode = 0;
		while ((errorCode = dataEntry.readData(&data[dataEntry.pos() - startOffset], 8016)) > 0 ) ;

		if (errorCode < 0)
			throw runtime_error("Error reading Entry " + entry + " in JASP archive.");

		Json::Reader jsonReader;
		string doc(data, size);
		jsonReader.parse(doc, root);
	}

	dataEntry.close();

	return true;
}

void JASPImporter::loadDataSet(DataSetPackage *packageData, const string &path, boost::function<void (const std::string &, int)> progressCallback)
{
	DataSet *dataSet = SharedMemory::createDataSet();
	bool success = false;

	Json::Value metaData;
	Json::Value analysesData;

	int columnCount = 0;
	int rowCount = 0;

	parseJsonEntry(metaData, path, "metadata.json", true);

	Json::Value &dataSetDesc = metaData["dataSet"];
	columnCount = dataSetDesc["columnCount"].asInt();
	rowCount = dataSetDesc["rowCount"].asInt();

	if (rowCount <= 0 || columnCount <= 0)
		throw runtime_error("Data size has been corrupted and cannot load.");

	do
	{
		try {

			success = true;

			dataSet->setColumnCount(columnCount);
			if (rowCount > 0)
				dataSet->setRowCount(rowCount);
		}
		catch (boost::interprocess::bad_alloc &e)
		{
			dataSet = SharedMemory::enlargeDataSet(dataSet);
			success = false;
		}
		catch (exception e)
		{
			cout << "n " << e.what();
			cout.flush();
		}
		catch (...)
		{
			cout << "something else\n ";
			cout.flush();
		}
	}
	while ( ! success);

	Json::Value &columnsDesc = dataSetDesc["fields"];
	int i = 0;
	for (Json::ValueIterator itr = columnsDesc.begin(); itr != columnsDesc.end(); itr++)
	{
		Json::Value columnDesc = (*itr);

		Column &column = dataSet->column(i);

		column.setName(columnDesc["name"].asString());
		column._columnType = getColumnType(columnDesc["measureType"].asString());

		Json::Value &labelsDesc = columnDesc["labels"];
		Labels &labels = column.labels();

		for (Json::Value::iterator iter = labelsDesc.begin(); iter != labelsDesc.end(); iter++)
		{
			Json::Value keyValuePair = *iter;
			int zero = 0;
			int key = keyValuePair.get(zero, Json::nullValue).asInt();
			labels.add(key, keyValuePair.get(1, Json::nullValue).asString());
		}

		i += 1;
	}

	unsigned long long progress;
	unsigned long long lastProgress = -1;


	string entryName = "data.bin";
	FileReader dataEntry = FileReader(path, entryName);
	if (!dataEntry.exists())
		throw runtime_error("Entry " + entryName + " cannot be found in JASP archive.");

	char buff[sizeof(double) > sizeof(int) ? sizeof(double) : sizeof(int)];

	for (int c = 0; c < columnCount; c++)
	{
		Column &column = dataSet->column(c);
		Column::ColumnType columnType = column.columnType();
		int typeSize = (columnType == Column::ColumnTypeScale) ? sizeof(double) : sizeof(int);
		for (int r = 0; r < rowCount; r++)
		{
			int size = dataEntry.readData(buff, typeSize);
			if (size != typeSize)
				throw runtime_error("Error reading data.bin from JASP archive.");

			if (columnType == Column::ColumnTypeScale)
			{
				column.setValue(r, *(double*)buff);
			}
			else
			{
				column.setValue(r, *(int*)buff);
			}

			progress = 100 * ((c * rowCount) + (r + 1)) / (columnCount * rowCount);
			if (progress != lastProgress)
			{
				progressCallback("Loading Data Set", progress);
				lastProgress = progress;
			}
		}
	}
	dataEntry.close();


	string entryName3 = "results.html";
	FileReader dataEntry3 = FileReader(path, entryName3);
	if (dataEntry3.exists())
	{
		int size1 = dataEntry3.bytesAvailable();
		char memblock1[size1];
		int startOffset1 = dataEntry3.pos();
		int errorCode = 0;
		while ((errorCode = dataEntry3.readData(&memblock1[dataEntry3.pos() - startOffset1], 8016)) > 0 ) ;
		if (errorCode < 0)
			throw runtime_error("Error reading Entry " + entryName3 + " in JASP archive.");

		packageData->analysesHTML = string(memblock1, size1);
		packageData->hasAnalyses = true;

		dataEntry3.close();
	}

	parseJsonEntry(analysesData, path, "analyses.json", false);

	vector<string> resources = FileReader::getEntryPaths(path, "resources");

	for (vector<string>::iterator iter = resources.begin(); iter != resources.end(); iter++)
	{
		string resource = *iter;

		FileReader resourceEntry = FileReader(path, resource);

		string filename = resourceEntry.fileName();
		string dir = resource.substr(0, resource.length() - filename.length() - 1);

		string destination = tempfiles_createSpecific(dir, resourceEntry.fileName());

		boost::nowide::ofstream file(destination.c_str(),  ios::out | ios::binary);

		char copyBuff[8016];
		int bytes = 0;
		while ((bytes = resourceEntry.readData(copyBuff, sizeof(copyBuff))) > 0 ) {
			file.write(copyBuff, bytes);
		}
		file.flush();
		file.close();

		if (bytes < 0)
			throw runtime_error("Error reading resource in JASP archive.");
	}

	packageData->analysesData = analysesData;
	packageData->hasAnalyses = true;


	packageData->dataSet = dataSet;
}

Column::ColumnType JASPImporter::getColumnType(string name)
{
	if (name == "Nominal")
		return  Column::ColumnTypeNominal;
	else if (name == "NominalText")
		return  Column::ColumnTypeNominalText;
	else if (name == "Ordinal")
		return  Column::ColumnTypeOrdinal;
	else if (name == "Continuous")
		return  Column::ColumnTypeScale;
	else
		return  Column::ColumnTypeUnknown;
}

