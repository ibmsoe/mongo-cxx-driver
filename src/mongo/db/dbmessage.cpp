// dbmessage.cpp

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/pch.h"

#include "mongo/db/dbmessage.h"

namespace mongo {

    string Message::toString() const {
        stringstream ss;
        ss << "op: " << opToString( operation() ) << " len: " << size();
        if ( operation() >= 2000 && operation() < 2100 ) {
            DbMessage d(*this);
            ss << " ns: " << d.getns();
            switch ( operation() ) {
            case dbUpdate: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                BSONObj o = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q << " update: " << o;
                break;
            }
            case dbInsert:
                ss << d.nextJsObj();
                break;
            case dbDelete: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q;
                break;
            }
            default:
                ss << " CANNOT HANDLE YET";
            }


        }
        return ss.str();
    }

    DbMessage::DbMessage(const Message& msg) : _msg(msg), _nsStart(NULL), _mark(NULL), _nsLen(0) {
        // for received messages, Message has only one buffer
        _theEnd = _msg.singleData()->_data + _msg.singleData()->dataLen();
        _nextjsobj = _msg.singleData()->_data;

		_reserved = readAndAdvance<int>();

        // Read packet for NS
        if (messageShouldHaveNs()) {

            // Limit = buffer size of message -
            //        (first int4 in message which is either flags or a zero constant)
            size_t limit = _msg.singleData()->dataLen() - sizeof(int);

            _nsStart = _nextjsobj;
            _nsLen = strnlen(_nsStart, limit);

            // Validate there is room for a null byte in the buffer
            // Strings can be zero length
            uassert(18633, "Failed to parse ns string", _nsLen <= (limit - 1));

            _nextjsobj += _nsLen + 1; // skip namespace + null
        }
    }

    const char * DbMessage::getns() const {
        verify(messageShouldHaveNs());
        return _nsStart;
    }
	
    #if 0
    int DbMessage::getQueryNToReturn() const {
        verify(messageShouldHaveNs());
        const char* p = _nsStart + _nsLen + 1;
        checkRead<int>(p, 2);

        //return ((reinterpret_cast<const int*>(p)))[1];
        return ((little<int>::ref(p)))[1];
    }
    #endif


    const char * DbMessage::afterNS() const {
            return _nsStart + _nsLen + 1;
        }

	int DbMessage::getInt( int num ) const {
            const little<int>* foo = &little<int>::ref( afterNS() );
            return foo[num];
        }

	int DbMessage::getQueryNToReturn() const {
            return getInt( 1 );
        }


    int DbMessage::pullInt() {
        return readAndAdvance<int>();

 
    }

    long long DbMessage::pullInt64() {
        return readAndAdvance<long long>();

    }

    const long long* DbMessage::getArray(size_t count) const {
        checkRead<long long>(_nextjsobj, count);
        return reinterpret_cast<const long long*>(_nextjsobj);
    }

    BSONObj DbMessage::nextJsObj() {
        massert(10304,
            "Client Error: Remaining data too small for BSON object",
            _nextjsobj != NULL && _theEnd - _nextjsobj >= 5);

        if (serverGlobalParams.objcheck) {
            Status status = validateBSON(_nextjsobj, _theEnd - _nextjsobj);
            massert(10307,
                str::stream() << "Client Error: bad object in message: " << status.reason(),
                status.isOK());
        }

        BSONObj js(_nextjsobj);
        verify(js.objsize() >= 5);
        verify(js.objsize() <= (_theEnd - _nextjsobj));

        _nextjsobj += js.objsize();
        if (_nextjsobj >= _theEnd)
            _nextjsobj = NULL;
        return js;
    }

    void DbMessage::markReset(const char * toMark = NULL) {
        if (toMark == NULL) {
            toMark = _mark;
        }

        verify(toMark);
        _nextjsobj = toMark;
    }

    template<typename T>
    void DbMessage::checkRead(const char* start, size_t count) const {
        if ((_theEnd - start) < static_cast<int>(sizeof(T) * count)) {
            uassert(18634, "Not enough data to read", false);
        }
    }

    template<typename T>
    T DbMessage::read() const {
        checkRead<T>(_nextjsobj, 1);

        return little<T>::ref(_nextjsobj);
    }

    template<typename T> T DbMessage::readAndAdvance() {
        T t = read<T>();
        _nextjsobj += sizeof(T);
        return t;
    }

    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      void *data, int size,
                      int nReturned, int startingFrom,
                      long long cursorId 
                      ) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        b.appendBuf(data, size);
        QueryResult *qr = (QueryResult *) b.buf();
        qr->_resultFlags() = queryResultFlags;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->cursorId = cursorId;
        qr->startingFrom = startingFrom;
        qr->nReturned = nReturned;
        b.decouple();
        Message resp(qr, true);
        p->reply(requestMsg, resp, requestMsg.header()->id);
    }

    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      const BSONObj& responseObj) {
        replyToQuery(queryResultFlags,
                     p, requestMsg,
                     (void *) responseObj.objdata(), responseObj.objsize(), 1);
    }

    void replyToQuery( int queryResultFlags, Message &m, DbResponse &dbresponse, BSONObj obj ) {
        Message *resp = new Message();
        replyToQuery( queryResultFlags, *resp, obj );
        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
    }

    void replyToQuery( int queryResultFlags, Message& response, const BSONObj& resultObj ) {
        BufBuilder bufBuilder;
        bufBuilder.skip( sizeof( QueryResult ));
        bufBuilder.appendBuf( reinterpret_cast< void *>(
                const_cast< char* >( resultObj.objdata() )), resultObj.objsize() );

        QueryResult* queryResult = reinterpret_cast< QueryResult* >( bufBuilder.buf() );
        bufBuilder.decouple();

        queryResult->_resultFlags() = queryResultFlags;
        queryResult->len = bufBuilder.len();
        queryResult->setOperation( opReply );
        queryResult->cursorId = 0;
        queryResult->startingFrom = 0;
        queryResult->nReturned = 1;

        response.setData( queryResult, true ); // transport will free
    }

}
