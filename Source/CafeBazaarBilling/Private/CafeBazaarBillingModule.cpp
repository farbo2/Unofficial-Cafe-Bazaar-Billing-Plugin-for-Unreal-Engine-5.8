#include "Modules/ModuleManager.h"

class FCafeBazaarBillingModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FCafeBazaarBillingModule, CafeBazaarBilling)
