import '../data/transaction_store.dart';

Future<TransactionStore> openApplicationStore() async =>
    MemoryTransactionStore();
