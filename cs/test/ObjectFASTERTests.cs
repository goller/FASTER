﻿// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

using System;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using System.Linq;
using FASTER.core;
using System.IO;
using NUnit.Framework;

namespace FASTER.test
{

    [TestFixture]
    internal class ObjectFASTERTests
    {
        private FasterKV<MyKey, MyValue, MyInput, MyOutput, Empty, MyFunctions> fht;
        private IDevice log, objlog;
        
        [SetUp]
        public void Setup()
        {
            log = Devices.CreateLogDevice(TestContext.CurrentContext.TestDirectory + "\\hlog2.log", deleteOnClose: true);
            objlog = Devices.CreateLogDevice(TestContext.CurrentContext.TestDirectory + "\\hlog2.obj.log", deleteOnClose: true);

            fht = new FasterKV<MyKey, MyValue, MyInput, MyOutput, Empty, MyFunctions>
                (128, new MyFunctions(),
                logSettings: new LogSettings { LogDevice = log, ObjectLogDevice = objlog, MutableFraction = 0.1, MemorySizeBits = 15, PageSizeBits = 10 },
                checkpointSettings: new CheckpointSettings { CheckPointType = CheckpointType.FoldOver },
                serializerSettings: new SerializerSettings<MyKey, MyValue> { keySerializer = () => new MyKeySerializer(), valueSerializer = () => new MyValueSerializer() }
                );
            fht.StartSession();
        }

        [TearDown]
        public void TearDown()
        {
            fht.StopSession();
            fht.Dispose();
            fht = null;
            log.Close();
        }

        [Test]
        public void ObjectInMemWriteRead()
        {
            var key1 = new MyKey { key = 9999999 };
            var value = new MyValue { value = 23 };

            MyInput input = null;
            MyOutput output = new MyOutput();

            fht.Upsert(ref key1, ref value, Empty.Default, 0);
            fht.Read(ref key1, ref input, ref output, Empty.Default, 0);
            Assert.IsTrue(output.value.value == value.value);
        }

        [Test]
        public void ObjectInMemWriteRead2()
        {
            var key1 = new MyKey { key = 8999998 };
            var input1 = new MyInput { value = 23 };
            MyOutput output = new MyOutput();

            fht.RMW(ref key1, ref input1, Empty.Default, 0);

            var key2 = new MyKey { key = 8999999 };
            var input2 = new MyInput { value = 24 };
            fht.RMW(ref key2, ref input2, Empty.Default, 0);

            fht.Read(ref key1, ref input1, ref output, Empty.Default, 0);

            Assert.IsTrue(output.value.value == input1.value);

            fht.Read(ref key2, ref input2, ref output, Empty.Default, 0);
            Assert.IsTrue(output.value.value == input2.value);

        }


        [Test]
        public void ObjectDiskWriteRead()
        {
            for (int i = 0; i < 2000; i++)
            {
                var key = new MyKey { key = i };
                var value = new MyValue { value = i };
                fht.Upsert(ref key, ref value, Empty.Default, 0);
                // fht.ShiftReadOnlyAddress(fht.LogTailAddress);
            }

            var key2 = new MyKey { key = 23 };
            var input = new MyInput();
            MyOutput g1 = new MyOutput();
            var status = fht.Read(ref key2, ref input, ref g1, Empty.Default, 0);

            if (status == Status.PENDING)
            {
                fht.CompletePending(true);
            }
            else
            {
                Assert.IsTrue(status == Status.OK);
            }

            Assert.IsTrue(g1.value.value == 23);

            key2 = new MyKey { key = 99999 };
            status = fht.Read(ref key2, ref input, ref g1, Empty.Default, 0);

            if (status == Status.PENDING)
            {
                fht.CompletePending(true);
            }
            else
            {
                Assert.IsTrue(status == Status.NOTFOUND);
            }
        }
    }
}
