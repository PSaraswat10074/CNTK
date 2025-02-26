//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#pragma once

#include "DataDeserializer.h"

namespace Microsoft { namespace MSR { namespace CNTK {

// Base class for data deserializers.
// Has a default implementation for a subset of methods.
class DataDeserializerBase : public IDataDeserializer
{
public:
    DataDeserializerBase() : m_sequencesInitialized(false)
    {}

    // Provides description of all sequences the deserializer can produce.
    const SequenceDescriptions& GetSequenceDescriptions() const override
    {
        if (!m_sequencesInitialized)
        {
            FillSequenceDescriptions(m_sequences);
            m_sequencesInitialized = true;
        }
        return m_sequences;
    }

protected:
    // Fills the timeline with sequence descriptions.
    // Inherited classes should provide the complete Sequence descriptions for all input data.
    virtual void FillSequenceDescriptions(SequenceDescriptions& timeline) const = 0;

    // Streams this data deserializer can produce.
    std::vector<StreamDescriptionPtr> m_streams;

private:
    DataDeserializerBase(const DataDeserializerBase&) = delete;
    DataDeserializerBase& operator=(const DataDeserializerBase&) = delete;

    mutable SequenceDescriptions m_sequences;
    mutable bool m_sequencesInitialized;
};

}}}
