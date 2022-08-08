#include "reporter.h"
#include "results/resultsjsinterface.h"
#include "data/datasetpackage.h"
#include "gui/messageforwarder.h"
#include "gui/preferencesmodel.h"
#include "analysis/analyses.h"
#include <iostream>
#include <QDateTime>
#include <QFile>

Reporter::Reporter(QObject *parent, QDir reportingDir) 
	: _reportingDir(reportingDir), 
	  _pdfPath(_reportingDir.absoluteFilePath("report.pdf"))
{
	QObject::connect(ResultsJsInterface::singleton(), &ResultsJsInterface::pdfPrintingFinished, this, &Reporter::onPdfPrintingFinishedHandler, Qt::UniqueConnection);
	//because of the connection exporting to pdf from the filemenu/results won't work anymore... 
	//but this is only used when JASP is running in reporting mode so that doesnt matter
}

bool Reporter::isJaspFileNotDabaseOrSynching() const
{
	//We report through cerr because otherwise it might get messy if JASP is started hidden from a service.
	//The service will just have to catch the output from std::cerr
	if(!DataSetPackage::pkg()->isLoaded() || !DataSetPackage::pkg()->hasDataSet())
	{
		std::cerr << "There is no file loaded or it has no data..." << std::endl;
		return false;
	}
	
	if(DataSetPackage::pkg()->isDatabase())
	{
		if(!DataSetPackage::pkg()->isDatabaseSynching())
		{
			std::cerr << "A non synching database file was loaded, which means there will be no updates..." << std::endl;
			return false;		
		}
	}
	else
	{
		if(DataSetPackage::pkg()->dataFilePath() == "")
		{
			std::cerr << "A jasp file without a datafile was loaded, which means there can be no updates..." << std::endl;
			return false;		
		}	
		
		//Lets make sure it actually checks whether the datafile is  being synched or not.
		PreferencesModel::prefs()->setDataAutoSynchronization(true);
	}
			
	return true;
}

void Reporter::analysesFinished()
{
	//The order in which the following are executed is important.
	//The report service will use the "modified" dates of the files created by each of these to keep an eye on JASP.
	//so, when "checkReports() write "sometingToReport.txt" the service knows a refresh has just completed
	//and when "report.complete" is written it knows everything is ready to be sent/read/whatever
	checkReports();
	writeResultsJson();
	writeReport();
}

/// Goes through all analyses' results and extracts those parts generated by jaspReport and assings as an array to _reports
bool Reporter::checkReports()
{
	bool		somethingToReport	= false;
	Json::Value	reports				= Json::arrayValue;
	
	std::function<void(stringvec & names, const Json::Value & meta)> reportNameExtractor = 			
		[&reportNameExtractor](stringvec & names, const Json::Value & meta) -> void
		{
			switch(meta.type())
			{
			case Json::arrayValue:
				for(const Json::Value & subEntry : meta)
					reportNameExtractor(names, subEntry);
				break;
				
			case Json::objectValue:
			{
				if(meta.get("type", "").asString() == "reportNode")
					names.push_back(meta["name"].asString());
				
				const Json::Value & collection = meta.get("collection", Json::arrayValue);
				
				for(const Json::Value & subEntry : collection)
					reportNameExtractor(names, subEntry);
				
				break;
			}
				
			default:
				//Why would we even be here? It is just to shut up the warning ;)
				break;
			}
		};
	
	std::function<void(const stringset & names, const Json::Value & results, Json::Value & reports)> reportsExtractor = 			
		[&reportsExtractor, &somethingToReport](const stringset & names, const Json::Value & results, Json::Value & reports) -> void
		{
			switch(results.type())
			{
			case Json::arrayValue:
				for(const Json::Value & subEntry : results)
					reportsExtractor(names, subEntry, reports);
				break;
				
			case Json::objectValue:
			{
				for(const std::string & name : results.getMemberNames())
					if(names.count(name))
					{
						const Json::Value & report = results[name];
						if(report["report"].asBool())
							somethingToReport = true;
						
						reports.append(report);
					}
					else if(name != ".meta" && results[name].isObject())
					{
						std::cerr << "looking at name " << name << std::endl;
						const Json::Value & collection = results[name].get("collection", Json::arrayValue);
				
						for(const Json::Value & subEntry : collection)
							reportsExtractor(names, results, reports);
					}
																			 
				break;
			}
				
			default:
				//Why would we even be here? It is just to shut up the warning ;)
				break;
			}
		};
	
	Analyses::analyses()->applyToAll([&](Analysis * a)
	{
		Json::Value analysisReports = Json::arrayValue;
		
		stringvec reportNames;
		reportNameExtractor(reportNames, a->resultsMeta());		
		
		reportsExtractor(stringset(reportNames.begin(), reportNames.end()), a->results(), analysisReports);
		
		if(analysisReports.size() || a->isErrorState())
		{
			Json::Value analysisReport = Json::objectValue;
			
			analysisReport["id"]		= int(a->id());
			analysisReport["name"]		= a->name();
			analysisReport["title"]		= a->title();
			analysisReport["module"]	= a->module();
			analysisReport["reports"]	= analysisReports;
			
			if(a->isErrorState())
			{
				analysisReport["error"]			= a->results().get("error", false).asBool();
				analysisReport["errorMessage"]	= a->results().get("errorMessage", "???").asString();
				//lets keep it concise analysisReport["results"]		= a->results();
			}
			
			reports.append(analysisReport);
		}
	});
	
	_reports = reports;
	
	QFile somethingToReportFile(_reportingDir.absoluteFilePath("reportSomething.txt"));
	
	if(somethingToReportFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		somethingToReportFile.write(somethingToReport ? "true" : "false"); //Just something simple to check, could be anything really.

	return somethingToReport;
}

void Reporter::writeResultsJson()
{
	QFile resultsFile(_reportingDir.absoluteFilePath("results.json"));
	
	if(resultsFile.open(QIODevice::WriteOnly | QIODevice::Truncate  | QIODevice::Text))
		resultsFile.write(_reports.toStyledString().c_str());
}

void Reporter::writeReport()
{
	ResultsJsInterface::singleton()->exportToPDF(_pdfPath);
}


///Called from ResultsJSInterface and before that WebEngine and makes sure "report.complete" is written/updated to signal to the service some new data is available
void Reporter::onPdfPrintingFinishedHandler(QString pdfPath)
{
	if(_pdfPath != pdfPath) 
	{
		std::cerr << "Got unexpected Reporter::onPdfPrintingFinishedHandler! Expected path: \"" << _pdfPath << "\" but got: \"" << pdfPath << "\"...\nIgnoring it!" << std::endl;
		return;
	}
	
	QFile reportComplete(_reportingDir.absoluteFilePath("report.complete"));
	
	if(reportComplete.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		reportComplete.write(QDateTime::currentDateTime().toUTC().toString(Qt::ISODate).toStdString().c_str());
}
