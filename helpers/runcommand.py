import os
from pymongo import MongoClient
import argparse
import datetime


def main(coll):
    parser = argparse.ArgumentParser()
    parser.add_argument('--command', choices='arm stop start'.split(), required=True, help='The command')
    parser.add_argument('--number', type=int, default=1, help='Run number')
    parser.add_argument('--mode', help='Run mode', required=True)
    parser.add_argument('--host', nargs='+', default=[os.uname()[1]], help="Hosts to issue to")

    args = parser.parse_args()

    doc = {
            "command": args.command,
            "number": args.number,
            "mode": args.mode,
            "host": args.host,
            "user": os.getlogin(),
            "run_identifier": '%06i' % args.number,
            "createdAt": datetime.datetime.utcnow()
            }
    coll.insert_one(doc)
    return

if __name__ == '__main__':
    with MongoClient("mongodb://daq:%s@xenon1t-daq:27017/admin" % os.environ['MONGO_PASSWORD_DAQ']) as client:
        try:
            main(client['daq']['control'])
        except Exception as e:
            print('%s: %s' % (type(e), e))