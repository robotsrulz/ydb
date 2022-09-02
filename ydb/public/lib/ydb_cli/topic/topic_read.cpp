#include "topic_metadata_fields.h"
#include "topic_read.h"
#include "topic_util.h"
#include "topic_write.h"
#include <library/cpp/json/json_reader.h>
#include <library/cpp/string_utils/base64/base64.h>
#include <util/generic/set.h>

namespace NYdb::NConsoleClient {
    namespace {
        constexpr i64 MessagesLimitUnlimited = -1;
        constexpr i64 MessagesLimitDefaultPrettyFormat = 10;
        constexpr i64 MessagesLimitDefaultJsonArrayFormat = 500;

        bool IsStreamingFormat(EMessagingFormat format) {
            return format == EMessagingFormat::NewlineDelimited || format == EMessagingFormat::Concatenated;
        }
    }

    TTopicReaderSettings::TTopicReaderSettings() {
    }

    TTopicReaderSettings::TTopicReaderSettings(
        TMaybe<i64> limit,
        bool commit,
        bool wait,
        EMessagingFormat format,
        TVector<ETopicMetadataField> metadataFields,
        ETransformBody transform,
        TDuration idleTimeout)
        : MetadataFields_(metadataFields)
        , IdleTimeout_(idleTimeout)
        , MessagingFormat_(format)
        , Transform_(transform)
        , Limit_(limit)
        , Commit_(commit)
        , Wait_(wait) {
    }

    TTopicReader::TTopicReader(
        std::shared_ptr<NTopic::IReadSession> readSession,
        TTopicReaderSettings params)
        : ReadSession_(readSession)
        , ReaderParams_(params) {
    }

    void TTopicReader::Init() {
        TVector<TString> tableFields;
        for (const auto& f : ReaderParams_.MetadataFields()) {
            tableFields.emplace_back(TStringBuilder() << f);
        }
        TPrettyTable table(tableFields);
        OutputTable_ = std::make_unique<TPrettyTable>(table);

        if (!ReaderParams_.Limit().Defined()) {
            if (IsStreamingFormat(ReaderParams_.MessagingFormat())) {
                MessagesLeft_ = MessagesLimitUnlimited;
            }
            if (ReaderParams_.MessagingFormat() == EMessagingFormat::Pretty) {
                MessagesLeft_ = MessagesLimitDefaultPrettyFormat;
            }
            if (ReaderParams_.MessagingFormat() == EMessagingFormat::JsonArray) {
                MessagesLeft_ = MessagesLimitDefaultJsonArrayFormat;
            }
            return;
        }

        i64 limit = *(ReaderParams_.Limit());
        if (IsStreamingFormat(ReaderParams_.MessagingFormat()) && limit == 0) {
            limit = -1;
        }
        MessagesLeft_ = limit;
    }

    namespace {
        const TString FormatBody(const TString& body, ETransformBody transform) {
            if (transform == ETransformBody::Base64) {
                return Base64Encode(body);
            }

            return body;
        }

        using TReceivedMessage = NTopic::TReadSessionEvent::TDataReceivedEvent::TMessage;

        void AddMetadataFieldToRow(TPrettyTable::TRow& row, const TReceivedMessage& message, ETransformBody transform, ETopicMetadataField f, size_t idx) {
            switch (f) {
                case ETopicMetadataField::Body:
                    row.Column(idx, FormatBody(message.GetData(), transform));
                    break;
                case ETopicMetadataField::CreateTime:
                    row.Column(idx, message.GetCreateTime());
                    break;
                case ETopicMetadataField::MessageGroupID:
                    row.Column(idx, message.GetMessageGroupId());
                    break;
                case ETopicMetadataField::Offset:
                    row.Column(idx, message.GetOffset());
                    break;
                case ETopicMetadataField::WriteTime:
                    row.Column(idx, message.GetWriteTime()); // improve for pretty
                    break;
                case ETopicMetadataField::SeqNo:
                    row.Column(idx, message.GetSeqNo());
                    break;
                case ETopicMetadataField::Meta:
                    NJson::TJsonValue json;
                    for (auto const& [k, v] : message.GetMeta()->Fields) {
                        json[k] = v;
                    }
                    row.Column(idx, json);
                    break;
            }
        }
    } // namespace

    void TTopicReader::PrintMessagesInPrettyFormat(IOutputStream& output) {
        for (const auto& message : ReceivedMessages_) {
            TPrettyTable::TRow& row = OutputTable_->AddRow();
            for (size_t i = 0; i < ReaderParams_.MetadataFields().size(); ++i) {
                ETopicMetadataField f = ReaderParams_.MetadataFields()[i];
                AddMetadataFieldToRow(row, message, ReaderParams_.Transform(), f, i);
            }
        }

        OutputTable_->Print(output);
    }

    void TTopicReader::PrintMessagesInJsonArrayFormat(IOutputStream& output) {
        // TODO(shmel1k@): not implemented yet.
        Y_UNUSED(output);
    }

    void TTopicReader::Close(IOutputStream& output, TDuration closeTimeout) {
        if (ReaderParams_.MessagingFormat() == EMessagingFormat::Pretty) {
            PrintMessagesInPrettyFormat(output);
        }
        if (ReaderParams_.MessagingFormat() == EMessagingFormat::JsonArray) {
            PrintMessagesInJsonArrayFormat(output);
        }
        output.Flush();
        bool success = ReadSession_->Close(closeTimeout);
        if (!success) {
            throw yexception() << "Failed to close read session\n";
        }
    }

    void TTopicReader::HandleReceivedMessage(const TReceivedMessage& message, IOutputStream& output) {
        EMessagingFormat MessagingFormat = ReaderParams_.MessagingFormat();
        if (MessagingFormat == EMessagingFormat::SingleMessage || MessagingFormat == EMessagingFormat::Concatenated) {
            output << FormatBody(message.GetData(), ReaderParams_.Transform());
            output.Flush();
            return;
        }
        if (MessagingFormat == EMessagingFormat::NewlineDelimited) {
            output << FormatBody(message.GetData(), ReaderParams_.Transform());
            output << "\n";
            output.Flush();
            return;
        }
        if (MessagingFormat == EMessagingFormat::SingleMessage) {
            output << FormatBody(message.GetData(), ReaderParams_.Transform());
            return;
        }

        ReceivedMessages_.push_back(message);
    }

    int TTopicReader::HandleDataReceivedEvent(NYdb::NTopic::TReadSessionEvent::TDataReceivedEvent* event, IOutputStream& output) {
        event->GetPartitionSession()->RequestStatus();
        HasFirstMessage_ = true;

        NTopic::TDeferredCommit defCommit;
        for (const auto& message : event->GetMessages()) {
            HandleReceivedMessage(message, output);
            if (ReaderParams_.Commit()) {
                defCommit.Add(message);
            }

            if (MessagesLeft_ == MessagesLimitUnlimited) {
                continue;
            }

            --MessagesLeft_;
            if (MessagesLeft_ == 0) {
                break;
            }
        }

        if (ReaderParams_.Commit()) {
            defCommit.Commit();
        }
        LastMessageReceivedTs_ = Now();
        return EXIT_SUCCESS;
    }

    int TTopicReader::HandleStartPartitionSessionEvent(NYdb::NTopic::TReadSessionEvent::TStartPartitionSessionEvent* event) {
        event->Confirm();
        if (event->GetCommittedOffset() == event->GetEndOffset()) {
            ReadingStatus_ = EReadingStatus::PartitionWithoutData;
        } else {
            ReadingStatus_ = EReadingStatus::PartitionWithData;
        }

        return EXIT_SUCCESS;
    }

    int TTopicReader::HandleCommitOffsetAcknowledgementEvent(NTopic::TReadSessionEvent::TCommitOffsetAcknowledgementEvent* event) {
        Y_UNUSED(event);

        return EXIT_SUCCESS;
    }

    int TTopicReader::HandlePartitionSessionStatusEvent(NTopic::TReadSessionEvent::TPartitionSessionStatusEvent* event) {
        if (event->GetReadOffset() == event->GetCommittedOffset()) {
            ReadingStatus_ = EReadingStatus::PartitionWithoutData;
        } else {
            ReadingStatus_ = EReadingStatus::PartitionWithData;
        }

        return EXIT_SUCCESS;
    }

    int TTopicReader::HandleEvent(TMaybe<NYdb::NTopic::TReadSessionEvent::TEvent>& event, IOutputStream& output) {
        if (!event) {
            return 0;
        }

        if (auto* dataEvent = std::get_if<NTopic::TReadSessionEvent::TDataReceivedEvent>(&*event)) {
            return HandleDataReceivedEvent(dataEvent, output);
        } else if (auto* createPartitionStreamEvent = std::get_if<NTopic::TReadSessionEvent::TStartPartitionSessionEvent>(&*event)) {
            return HandleStartPartitionSessionEvent(createPartitionStreamEvent);
        } else if (auto* commitEvent = std::get_if<NTopic::TReadSessionEvent::TCommitOffsetAcknowledgementEvent>(&*event)) {
            return HandleCommitOffsetAcknowledgementEvent(commitEvent);
        } else if (auto* partitionStatusEvent = std::get_if<NTopic::TReadSessionEvent::TPartitionSessionStatusEvent>(&*event)) {
            return HandlePartitionSessionStatusEvent(partitionStatusEvent);
        } else if (auto* sessionClosedEvent = std::get_if<NTopic::TSessionClosedEvent>(&*event)) {
            ThrowOnError(*sessionClosedEvent);
            return 1;
        }
        return 0;
    }

    int TTopicReader::Run(IOutputStream& output) {
        LastMessageReceivedTs_ = TInstant::Now();

        bool waitForever = ReaderParams_.Wait() && (ReaderParams_.MessagingFormat() == EMessagingFormat::NewlineDelimited || ReaderParams_.MessagingFormat() == EMessagingFormat::Concatenated);

        while ((MessagesLeft_ > 0 || MessagesLeft_ == -1) && !IsInterrupted()) {
            TInstant messageReceiveDeadline = LastMessageReceivedTs_ + ReaderParams_.IdleTimeout();
            NThreading::TFuture<void> future = ReadSession_->WaitEvent();
            future.Wait(messageReceiveDeadline);
            if (!future.HasValue()) {
                if (waitForever) {
                    LastMessageReceivedTs_ = TInstant::Now();
                    continue;
                }

                bool isReading = ReadingStatus_ == EReadingStatus::PartitionWithData;
                if (!isReading || (isReading && HasFirstMessage_)) {
                    return EXIT_SUCCESS;
                }

                LastMessageReceivedTs_ = TInstant::Now();
                continue;
            }

            // TODO(shmel1k@): throttling?
            // TODO(shmel1k@): think about limiting size of events
            TMaybe<NYdb::NTopic::TReadSessionEvent::TEvent> event = ReadSession_->GetEvent(true);
            if (!event) {
                // TODO(shmel1k@): does it work properly?
                continue;
            }

            if (int status = HandleEvent(event, output); status) {
                return status;
            }
        }
        return EXIT_SUCCESS;
    }
} // namespace NYdb::NConsoleClient