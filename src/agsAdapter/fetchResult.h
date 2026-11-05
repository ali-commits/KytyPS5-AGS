#ifndef AGS_ADAPTER_FETCH_RESULT_H
#define AGS_ADAPTER_FETCH_RESULT_H

// The outcome of a cheat download that finished after its call timed out. It is handed to the
// next call for the same game within MaxAgeMs of completion and then forgotten, so an old result
// is never replayed instead of fetching.
#include <QString>

#include <optional>

struct FetchOutcome {
	QString game;
	QString error;   // empty on success
	QString message; // shown on success
};

class FetchResultCache {
public:
	static constexpr qint64 MaxAgeMs = 60 * 1000;

	void Store(FetchOutcome outcome, qint64 now_ms) {
		m_outcome  = std::move(outcome);
		m_finished = now_ms;
	}

	// The stored outcome for `game` when it finished at most MaxAgeMs ago. Any stored outcome is
	// consumed: returned once, or discarded when it is too old or belongs to another game.
	[[nodiscard]] std::optional<FetchOutcome> Take(const QString& game, qint64 now_ms) {
		auto outcome = std::move(m_outcome);
		m_outcome.reset();
		if (!outcome || outcome->game != game || now_ms - m_finished > MaxAgeMs)
			return std::nullopt;
		return outcome;
	}

private:
	std::optional<FetchOutcome> m_outcome;
	qint64                      m_finished = 0;
};

#endif
