#ifndef UTILITY_SINGLETON_H_
#define UTILITY_SINGLETON_H_

#define DECLARE_SINGLETON(ClassName) \
	private: \
		static ClassName *singleton_; \
		ClassName(); \
	public: \
		static ClassName *GetInstance(); \
		static void ReleaseInstance();

#define IMPLEMENT_SINGLETON(ClassName) \
	ClassName *ClassName::singleton_ = nullptr; \
	ClassName *ClassName::GetInstance() { \
		if (singleton_ == nullptr) { \
			singleton_ = new ClassName(); \
		} \
		return singleton_; \
	} \
	void ClassName::ReleaseInstance() { \
		if (singleton_ != nullptr) { \
			delete singleton_; \
			singleton_ = nullptr; \
		} \
	}

#endif /* UTILITY_SINGLETON_H_ */